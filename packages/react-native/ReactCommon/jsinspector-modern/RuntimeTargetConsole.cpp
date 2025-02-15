/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <jsinspector-modern/RuntimeTarget.h>

#include <deque>
#include <string>

using namespace facebook::jsi;
using namespace std::string_literals;

namespace facebook::react::jsinspector_modern {

namespace {

struct ConsoleState {
  /**
   * https://console.spec.whatwg.org/#counting
   */
  std::unordered_map<std::string, int> countMap;

  /**
   * https://console.spec.whatwg.org/#timing
   */
  std::unordered_map<std::string, double> timerTable;

  ConsoleState() = default;
  ConsoleState(const ConsoleState&) = delete;
  ConsoleState& operator=(const ConsoleState&) = delete;
  ConsoleState(ConsoleState&&) = delete;
  ConsoleState& operator=(ConsoleState&&) = delete;
  ~ConsoleState() = default;
};

/**
 * `console` methods that have no behaviour other than emitting a
 * Runtime.consoleAPICalled message.
 */
constexpr const std::pair<const char*, ConsoleAPIType>
    kForwardingConsoleMethods[] = {
        {"clear", ConsoleAPIType::kClear},
        {"debug", ConsoleAPIType::kDebug},
        {"dir", ConsoleAPIType::kDir},
        {"dirxml", ConsoleAPIType::kDirXML},
        {"error", ConsoleAPIType::kError},
        {"group", ConsoleAPIType::kStartGroup},
        {"groupCollapsed", ConsoleAPIType::kStartGroupCollapsed},
        {"groupEnd", ConsoleAPIType::kEndGroup},
        {"info", ConsoleAPIType::kInfo},
        {"log", ConsoleAPIType::kLog},
        {"table", ConsoleAPIType::kTable},
        {"trace", ConsoleAPIType::kTrace},
        {"warn", ConsoleAPIType::kWarning},
};

/**
 * JS `Object.create()`
 */
jsi::Object objectCreate(jsi::Runtime& runtime, jsi::Value prototype) {
  auto objectGlobal = runtime.global().getPropertyAsObject(runtime, "Object");
  auto createFn = objectGlobal.getPropertyAsFunction(runtime, "create");
  return createFn.callWithThis(runtime, objectGlobal, prototype)
      .getObject(runtime);
}

bool toBoolean(jsi::Runtime& runtime, const jsi::Value& val) {
  // Based on Operations.cpp:toBoolean in the Hermes VM.
  if (val.isUndefined() || val.isNull()) {
    return false;
  }
  if (val.isBool()) {
    return val.getBool();
  }
  if (val.isNumber()) {
    double m = val.getNumber();
    return m != 0 && !std::isnan(m);
  }
  if (val.isSymbol() || val.isObject()) {
    return true;
  }
  if (val.isString()) {
    std::string s = val.getString(runtime).utf8(runtime);
    return !s.empty();
  }
  assert(false && "All cases should be covered");
  return false;
}

/**
 * Get the current time in milliseconds as a double.
 */
double getTimestampMs() {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

void RuntimeTarget::installConsoleHandler() {
  jsExecutor_([selfWeak = weak_from_this(),
               selfExecutor = executorFromThis()](jsi::Runtime& runtime) {
    jsi::Value consolePrototype = jsi::Value::null();
    auto originalConsoleVal = runtime.global().getProperty(runtime, "console");
    std::shared_ptr<jsi::Object> originalConsole;
    if (originalConsoleVal.isObject()) {
      originalConsole =
          std::make_shared<jsi::Object>(originalConsoleVal.getObject(runtime));
      consolePrototype = std::move(originalConsoleVal);
    } else {
      consolePrototype = jsi::Object(runtime);
    }
    auto console = objectCreate(runtime, std::move(consolePrototype));
    auto state = std::make_shared<ConsoleState>();

    /**
     * An executor that runs synchronously and provides a safe reference to our
     * RuntimeTargetDelegate for use on the JS thread.
     * \see RuntimeTargetDelegate for information on which methods are safe to
     * call on the JS thread.
     * \warning The callback will not run if the RuntimeTarget has been
     * destroyed.
     */
    auto delegateExecutorSync = [selfWeak, selfExecutor](auto&& func) {
      if (auto self = selfWeak.lock()) {
        // Q: Why is it safe to use self->delegate_ here?
        // A: Because the caller of InspectorTarget::registerRuntime
        // is explicitly required to guarantee that the delegate not
        // only outlives the target, but also outlives all JS code
        // execution that occurs on the JS thread.
        func(self->delegate_);
        // To ensure we never destroy `self` on the JS thread, send
        // our shared_ptr back to the inspector thread.
        selfExecutor([self = std::move(self)](auto&) { (void)self; });
      }
    };

    /**
     * Call \param innerFn and forward any arguments to the original console
     * method named \param methodName, if possible.
     */
    auto forwardToOriginalConsole = [originalConsole, delegateExecutorSync](
                                        const char* methodName,
                                        auto&& innerFn) {
      return [originalConsole,
              delegateExecutorSync,
              innerFn = std::forward<decltype(innerFn)>(innerFn),
              methodName](
                 jsi::Runtime& runtime,
                 const jsi::Value& thisVal,
                 const jsi::Value* args,
                 size_t count) mutable {
        jsi::Value retVal = innerFn(runtime, thisVal, args, count);
        if (originalConsole) {
          auto val = originalConsole->getProperty(runtime, methodName);
          if (val.isObject()) {
            auto obj = val.getObject(runtime);
            if (obj.isFunction(runtime)) {
              auto func = obj.getFunction(runtime);
              func.callWithThis(runtime, *originalConsole, args, count);
            }
          }
        }
        return retVal;
      };
    };

    /**
     * Install a console method with the given name and body. The body receives
     * the usual JSI host function parameters plus a ConsoleState reference, a
     * reference to the RuntimeTargetDelegate for sending messages to the
     * client, and the timestamp of the call. After the body runs (or is skipped
     * due to RuntimeTarget having been destroyed), the method of the same name
     * is also called on originalConsole (if it exists).
     */
    auto installConsoleMethod =
        [&](const char* methodName,
            std::function<void(
                jsi::Runtime & runtime,
                const jsi::Value* args,
                size_t count,
                RuntimeTargetDelegate& runtimeTargetDelegate,
                ConsoleState& state,
                double timestampMs)>&& body) {
          console.setProperty(
              runtime,
              methodName,
              jsi::Function::createFromHostFunction(
                  runtime,
                  jsi::PropNameID::forAscii(runtime, methodName),
                  0,
                  forwardToOriginalConsole(
                      methodName,
                      [body = std::move(body), state, delegateExecutorSync](
                          jsi::Runtime& runtime,
                          const jsi::Value& /*thisVal*/,
                          const jsi::Value* args,
                          size_t count) mutable {
                        auto timestampMs = getTimestampMs();
                        delegateExecutorSync(
                            [&runtime,
                             args,
                             count,
                             body = std::move(body),
                             state,
                             timestampMs](auto& runtimeTargetDelegate) {
                              body(
                                  runtime,
                                  args,
                                  count,
                                  runtimeTargetDelegate,
                                  *state,
                                  timestampMs);
                            });
                        return jsi::Value::undefined();
                      })));
        };

    /**
     * console.count
     */
    installConsoleMethod(
        "count",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& state,
           auto timestampMs) {
          std::string label = "default";
          if (count > 0 && !args[0].isUndefined()) {
            label = args[0].toString(runtime).utf8(runtime);
          }
          auto it = state.countMap.find(label);
          if (it == state.countMap.end()) {
            it = state.countMap.insert({label, 1}).first;
          } else {
            it->second++;
          }
          std::vector<jsi::Value> vec;
          vec.emplace_back(jsi::String::createFromUtf8(
              runtime, label + ": "s + std::to_string(it->second)));
          runtimeTargetDelegate.addConsoleMessage(
              runtime, {timestampMs, ConsoleAPIType::kCount, std::move(vec)});
        });

    /**
     * console.countReset
     */
    installConsoleMethod(
        "countReset",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& state,
           auto timestampMs) {
          std::string label = "default";
          if (count > 0 && !args[0].isUndefined()) {
            label = args[0].toString(runtime).utf8(runtime);
          }
          auto it = state.countMap.find(label);
          if (it == state.countMap.end()) {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime, "Count for '"s + label + "' does not exist"));
            runtimeTargetDelegate.addConsoleMessage(
                runtime,
                {timestampMs, ConsoleAPIType::kWarning, std::move(vec)});
          } else {
            it->second = 0;
          }
        });

    /**
     * console.time
     */
    installConsoleMethod(
        "time",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& state,
           auto timestampMs) {
          std::string label = "default";
          if (count > 0 && !args[0].isUndefined()) {
            label = args[0].toString(runtime).utf8(runtime);
          }
          auto it = state.timerTable.find(label);
          if (it == state.timerTable.end()) {
            state.timerTable.insert({label, timestampMs});
          } else {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime, "Timer '"s + label + "' already exists"));
            runtimeTargetDelegate.addConsoleMessage(
                runtime,
                {timestampMs, ConsoleAPIType::kWarning, std::move(vec)});
          }
        });

    /**
     * console.timeEnd
     */
    installConsoleMethod(
        "timeEnd",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& state,
           auto timestampMs) {
          std::string label = "default";
          if (count > 0 && !args[0].isUndefined()) {
            label = args[0].toString(runtime).utf8(runtime);
          }
          auto it = state.timerTable.find(label);
          if (it == state.timerTable.end()) {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime, "Timer '"s + label + "' does not exist"));
            runtimeTargetDelegate.addConsoleMessage(
                runtime,
                {timestampMs, ConsoleAPIType::kWarning, std::move(vec)});
          } else {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime,
                label + ": "s + std::to_string(timestampMs - it->second) +
                    " ms"));
            state.timerTable.erase(it);
            runtimeTargetDelegate.addConsoleMessage(
                runtime,
                {timestampMs, ConsoleAPIType::kTimeEnd, std::move(vec)});
          }
        });

    /**
     * console.timeLog
     */
    installConsoleMethod(
        "timeLog",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& state,
           auto timestampMs) {
          std::string label = "default";
          if (count > 0 && !args[0].isUndefined()) {
            label = args[0].toString(runtime).utf8(runtime);
          }
          auto it = state.timerTable.find(label);
          if (it == state.timerTable.end()) {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime, "Timer '"s + label + "' does not exist"));
            runtimeTargetDelegate.addConsoleMessage(
                runtime,
                {timestampMs, ConsoleAPIType::kWarning, std::move(vec)});
          } else {
            std::vector<jsi::Value> vec;
            vec.emplace_back(jsi::String::createFromUtf8(
                runtime,
                label + ": "s + std::to_string(timestampMs - it->second) +
                    " ms"));
            if (count > 1) {
              for (size_t i = 1; i != count; ++i) {
                vec.emplace_back(runtime, args[i]);
              }
            }
            runtimeTargetDelegate.addConsoleMessage(
                runtime, {timestampMs, ConsoleAPIType::kLog, std::move(vec)});
          }
        });

    /**
     * console.assert
     */
    installConsoleMethod(
        "assert",
        [](jsi::Runtime& runtime,
           const jsi::Value* args,
           size_t count,
           RuntimeTargetDelegate& runtimeTargetDelegate,
           ConsoleState& /*state*/,
           auto timestampMs) {
          if (count >= 1 && toBoolean(runtime, args[0])) {
            return;
          }
          std::deque<jsi::Value> data;

          if (count > 1) {
            for (size_t i = 1; i != count; ++i) {
              data.emplace_back(runtime, args[i]);
            }
          }
          if (data.empty()) {
            data.emplace_back(
                jsi::String::createFromUtf8(runtime, "Assertion failed"));
          } else if (data.front().isString()) {
            data.front() = jsi::String::createFromUtf8(
                runtime,
                "Assertion failed: "s +
                    data.front().asString(runtime).utf8(runtime));
          } else {
            data.emplace_front(
                jsi::String::createFromUtf8(runtime, "Assertion failed"));
          }
          runtimeTargetDelegate.addConsoleMessage(
              runtime,
              {timestampMs,
               ConsoleAPIType::kAssert,
               std::vector<jsi::Value>(
                   make_move_iterator(data.begin()),
                   make_move_iterator(data.end()))});
        });

    for (auto& [name, type] : kForwardingConsoleMethods) {
      installConsoleMethod(
          name,
          [type = type](
              jsi::Runtime& runtime,
              const jsi::Value* args,
              size_t count,
              RuntimeTargetDelegate& runtimeTargetDelegate,
              ConsoleState& /*state*/,
              auto timestampMs) {
            std::vector<jsi::Value> argsVec;
            for (size_t i = 0; i != count; ++i) {
              argsVec.emplace_back(runtime, args[i]);
            }
            runtimeTargetDelegate.addConsoleMessage(
                runtime, {timestampMs, type, std::move(argsVec)});
          });
    }

    runtime.global().setProperty(runtime, "console", console);
  });
}

} // namespace facebook::react::jsinspector_modern

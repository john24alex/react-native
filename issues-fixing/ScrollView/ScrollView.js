import React from 'react';
import { StyleSheet, Text, View, ScrollView } from 'react-native';

class App extends React.Component {
  render() {
    return (
        // Create a root view to show the header and scroll view
      <View style={[{ borderWidth: 2, borderColor: 'black' }, styles.root]}>
        <Header headerText="ScrollView Example" />
        {/* Create a scroll view with some items  */}
        <ScrollView style={styles.scrollView} contentContainerStyle={styles.contentContainer}>
          {Array.from({ length: 10 }).map((_, index) => (
            <View key={index} style={styles.item}>
              <Text>Item {index + 1}</Text>
            </View>
          ))}
        </ScrollView>
      </View>
    );
  }
}

// Show header for scroll view
const Header = (props) => (
  <View style={[styles.header, { borderWidth: 2, borderColor: 'red', borderRadius: 5 }]}>
    <Text style={{ color: '#fff' }}>{props.headerText}</Text>
  </View>
);


// Style the header and scroll view
const styles = StyleSheet.create({
  root: {
    width: 360,
    height: 640,
    backgroundColor: 'blue',
  },
  header: {
    backgroundColor: '#545353',
    alignItems: 'center',
    justifyContent: 'center',
    height: 44,
    color: '#fff',
  },
  scrollView: {
    flex: 1,
    width: '100%',
  },
  contentContainer: {
    alignItems: 'center',
    paddingBottom: 0,
  },
  item: {
    backgroundColor: 'white',
    padding: 20,
    marginVertical: 10,
    width: '90%',
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: 5,
  },
});

export default App;

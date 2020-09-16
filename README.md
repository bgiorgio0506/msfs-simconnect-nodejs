# msfs-simconnect-nodejs
Microsoft Flight Simulator 2020 SimConnect SDK wrapper for NodeJS.

Works on 64 bit version of NodeJS. Currently, for Windows only. 

## Installation
msfs-simconnect-nodejs uses a native NodeJS addon and therefore, it must be compiled first before you can use it as module within your project.

### 1.) Install the Node module

`npm install msfs-simconnect-node`

### 2.) Copy your SimConnect SDK files
You need to copy your own SimConnect SDK files to msfs-simconnect-nodejs. 64 bit version is also supported.

Default, the SimConnect SDK installed in C:\MSFS SDK\SimConnect SDK. Copy this SimConnect SDK directory and paste it in the root of msfs-simconnect-nodejs.

### 3.) Compile
Compile the library:

`npm run build`

Once compiled, at the moment you need to copy your SimConnect.dll file from the SimConnect SDK directory into the build/Release directory. You need to this everytime you've done a rebuild.

## 4.) Requirements
* NodeJS 64 bit
* Microsoft Visual Studio 2019 (Community)
* Python 3 (for compilation)
* SimConnect SDK 

## 5.) Usage
Import the module:

`const simConnect = require('msfs-simconnect-nodejs');`

The available functions are described below. Please refer to [example.js](examples/nodejs/example.js) for more help.

### open
`open(connectedCallback, simExitedCallback, exceptionCallback, errorCallback)`

Open connection and provide callback functions for handling critical events. Returns `false` if it failed to call `open` (eg. if sim is not running).

**Example**
```javascript
var success = simConnect.open("MyAppName", 
    (name, version) => {
        console.log("Connected to: " + name + "\nSimConnect version: " + version);
        // Safe to start interacting with SimConnect here (request data, etc)
    }, () => {
        console.log("Simulator exited by user");
    }, (exception) => {
        console.log("SimConnect exception: " + exception.name + " (" + exception.dwException + ", " + exception.dwSendID + ", " + exception.dwIndex + ", " + exception.cbData + ")");
    }, (error) => {
        console.log("Undexpected disconnect/error: " + error); // Look up error code in ntstatus.h for details
});
```

### requestDataOnSimObject
`requestDataOnSimObject(reqData, callback, objectId, period, dataRequestFlag)`

Request one or more [Simulation Variables](https://msdn.microsoft.com/en-us/library/cc526981.aspx) and set a callback function to later handle the received data. See [SDK Reference](https://msdn.microsoft.com/en-us/library/cc526983.aspx#SimConnect_RequestDataOnSimObject) for more details.

Each simulation variable is defined by an array. 

**Example:**
```javascript
[
    "Plane Latitude",              // Datum name
    "degrees",                     // Units name
    simConnect.datatype.FLOAT64,   // Datum type (optional, FLOAT64 is default and works for most data types)
    0                              // Epsilon (optional, 0 is default)
]    
```
**Full example:**
```javascript
simConnect.requestDataOnSimObject([
        ["Plane Latitude", "degrees"],
        ["Plane Longitude", "degrees"],  
        ["PLANE ALTITUDE", "feet"]
    ], (data) => {
        // Called when data is received
        console.log(
            "Latitude:  " + data["Plane Latitude"] + "\n" +
            "Longitude: " + data["Plane Longitude"] + "\n" +
            "Altitude:  " + data["PLANE ALTITUDE"] + " feet"
        );
    }, 
    simConnect.objectId.USER,               // User aircraft
    simConnect.period.SIM_FRAME,            // Get data every sim frame...
    simConnect.dataRequestFlag.CHANGED      // ...but only if one of the variables have changed
);
```


### requestDataOnSimObjectType
`requestDataOnSimObjectType(reqData, callback, radius, simobjectType)`

Similar to `requestDataOnSimObject`. Used to retrieve information about simulation objects of a given type that are within a specified radius of the user's aircraft. See [SDK Reference](https://msdn.microsoft.com/en-us/library/cc526983.aspx#SimConnect_RequestDataOnSimObjectType) for more details.

**Example**:
This will receive info about the user's aircraft. For this, a radius of 0 is used. Notice that when `STRINGV` is requested, the unit should be `null`.
```javascript
simConnect.requestDataOnSimObjectType([
    ["NAV IDENT:1", null, simConnect.datatype.STRINGV],
    ["NAV NAME:1", null, simConnect.datatype.STRINGV],
    ["NAV DME:1","Nautical miles"],
], (data) => {
    console.log(data);
}, 0 /* radius=0 */, simConnect.simobjectType.USER);
```

**Example**:
This will receive info about all aircraft within a 10 km radius. The callback will run one time for each identified aircraft.
```javascript
simConnect.requestDataOnSimObjectType([
    ["ATC MODEL",null,simConnect.datatype.STRINGV],
    ["Plane Latitude", "degrees"],
    ["Plane Longitude", "degrees"]
], (data) => {
    console.log(data);
}, 10000, simConnect.simobjectType.AIRCRAFT);
```

### createDataDefinition
`createDataDefinition(reqData)`

Used to create a data definition. Returns an id which can be used with `requestDataOnSimObjectType` in place of the array. This should be used when you have multiple requests for the same data - otherwise the app will crash after receiving too many requests. 

**Example**:
```javascript
var navInfoDefId = simConnect.createDataDefinition([
    ["NAV DME:1", "Nautical miles"],
    ["NAV GLIDE SLOPE ERROR:1", "Degrees"],
    ["NAV RADIAL ERROR:1", "Degrees"],
]);

setInterval(() => {
    simConnect.requestDataOnSimObjectType(navInfoDefId, (data) => {
        console.log(data)
    }, 0, simConnect.simobjectType.USER)
},100)
```

### setDataOnSimObject
`setDataOnSimObject(variableName, unit, value)`

Set a single [Simulation Variable](https://msdn.microsoft.com/en-us/library/cc526981.aspx) on user aircraft. First parameter is the datum name, second is the units name and last is the value.

**Example**:
```javascript
simConnect.setDataOnSimObject("GENERAL ENG THROTTLE LEVER POSITION:1", "Percent", 50);
```

### subscribeToSystemEvent
`subscribeToSystemEvent(eventName, callback)`

Subscribe to a system event. See [SDK Reference](https://msdn.microsoft.com/en-us/library/cc526983.aspx#SimConnect_SubscribeToSystemEvent) for available events.

**Example**:
```javascript
simConnect.subscribeToSystemEvent("Pause", (paused) => { 
    // Called when the system event occurs
    console.log(paused ? "Sim paused" : "Sim un-paused");
});
```
### close
`close()`

Manually close the connection to SimConnect. Returns `false` if it fails.

**Example**:
```javascript
var success = simConnect.close();
```

## Thanks
Inspired by https://github.com/EvenAR/node-simconnect & https://github.com/CockpitConnect/msfs-simconnect-nodejs

## Licence
[MIT](https://opensource.org/licenses/MIT)

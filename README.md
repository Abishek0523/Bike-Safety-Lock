# 🚲 Smart Bike Security System

### An IoT-Based Motorcycle Security and GPS Tracking Solution

## 📌 About the Project

The **Smart Bike Security System** is an IoT-based security solution designed to improve motorcycle safety by combining embedded hardware, GPS tracking, GSM communication, and a web-based monitoring interface.

The system uses an ESP32 microcontroller to manage security operations, detect unauthorized access attempts, and support vehicle monitoring. When suspicious activity is detected, the system can trigger security responses and notify the owner.

A web interface provides a convenient way to visualize the motorcycle's location and monitor relevant security information.

The main goal is to develop an affordable and practical solution that helps motorcycle owners improve vehicle security and respond quickly to potential theft.

## 🎯 Project Objectives

* Improve motorcycle security using an ESP32-based embedded system.
* Detect unauthorized access through a passcode-based security mechanism.
* Alert the owner through GSM-based phone calls after repeated incorrect passcode attempts.
* Track the motorcycle's geographical location using GPS.
* Support ignition locking and a siren-based security alert.
* Display location information through a web-based monitoring interface.

## ✨ Key Features

### 🔐 1. Passcode-Based Security

The system uses a keypad to authenticate access. Repeated incorrect passcode attempts can trigger the security alert mechanism.

### 📍 2. GPS Location Tracking

A GPS module obtains geographical coordinates that can be used to identify the motorcycle's location.

### 📞 3. GSM Emergency Calling

A GSM module can initiate a phone call to the registered owner after the configured number of incorrect passcode attempts.

### 🚨 4. Theft Alert System

The system uses a siren to warn nearby people when a configured security event occurs.

### 🔒 5. Ignition Control

A relay-based mechanism controls the ignition circuit as part of the security system.

### 🗺️ 6. Web-Based Location Monitoring

The website provides a visual interface for displaying motorcycle location information on a map.

The map interface is intended to make geographical coordinates easier to understand and help the owner monitor the vehicle's reported location.

## 🛠️ Hardware Components

The project uses the following hardware components:

* ESP32 microcontroller
* NEO-6M GPS module
* SIM800L GSM module
* 4×4 keypad
* 16×2 LCD with I2C interface
* MPU6050 motion sensor
* Relay module
* Buzzer or siren
* Power supply and buck converter
* Connecting wires and supporting components

## 💻 Software and Technologies

### Embedded System

* Arduino IDE
* Embedded C/C++
* ESP32 development platform

### Web Interface

* HTML
* CSS
* JavaScript
* Interactive map integration

### Communication and Tracking

* GPS-based geographical coordinates
* GSM-based communication
* Serial communication between hardware modules

## ⚙️ How It Works

1. **System Initialization:** The ESP32 initializes the connected hardware modules.
2. **Passcode Authentication:** The user enters a passcode through the keypad.
3. **Security Monitoring:** The system checks authentication attempts and responds to configured security events.
4. **Unauthorized Access Alert:** After the configured number of incorrect attempts, the system can activate the alert mechanism and initiate a phone call to the owner.
5. **GPS Tracking:** The GPS module obtains location coordinates when a valid GPS fix is available.
6. **Ignition Protection:** The relay-based mechanism controls the ignition circuit according to the system's security logic.
7. **Website Monitoring:** The web interface displays available location information on an interactive map.

*Actual behavior depends on the hardware connections, firmware implementation, GPS signal availability, and website integration.*

## 🗺️ Website Overview

The web interface is designed to make the security system easier to monitor.

Its intended functionality includes:

* Displaying geographical location information.
* Visualizing coordinates on an interactive map.
* Providing a user-friendly monitoring interface.
* Supporting future improvements such as live location updates and security status indicators.

**Important:** A map displayed in the website does not automatically provide live tracking. Live updates require a working communication path between the ESP32 hardware and the website.

## 🏗️ System Architecture

The project consists of three main parts:

**1. Embedded Hardware Layer**
The ESP32 communicates with the keypad, GPS module, GSM module, motion sensor, LCD, and relay.

**2. Communication Layer**
The hardware collects sensor information, obtains GPS coordinates, and communicates security alerts through the GSM module.

**3. Web Monitoring Layer**
The website presents available location information through a browser-based interface.

Together, these components form an integrated motorcycle security and monitoring solution.

## 🚀 Future Improvements

* Real-time GPS tracking with automatic map updates.
* Secure communication between the ESP32 and the website.
* Live security status and alert notifications.
* Historical location tracking.
* Mobile-friendly monitoring dashboard.
* Improved power management and hardware protection.
* Authentication and access control for the web dashboard.

## 🎓 Academic Project

This project was developed as part of an **Embedded and Cyber-Physical Systems (ECS)** academic project.

It demonstrates the practical application of embedded systems, IoT concepts, GPS navigation, GSM communication, hardware interfacing, and web technologies to solve a real-world vehicle security problem.

## 👨‍💻 Contributors

This project was developed collaboratively by our student team.

* **Embedded System:** Hardware assembly and microcontroller integration.
* **Website Development:** Web interface and map integration.
* **Documentation:** Project documentation and technical records.
* **Presentation:** Research, presentation, and project materials.

## 🤝 Contributions

Suggestions and improvements are welcome. Feel free to explore the repository and share ideas that could improve the system's security, reliability, and usability.

## 📜 License

A license can be added to define how others may use, modify, and distribute this project.

---

**Built with ❤️ using ESP32, GPS, GSM, and Web Technologies.**

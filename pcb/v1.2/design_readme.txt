All ESP32-Inputs connecting to Davis sensors are ESD protected (should protect against nearby lightning). 

i2c bus for bme280 sensor:
– Additional 4k7 pullups can be activated by jumper J1 and J2 for longer (max. 2 meter) distances. ESP32 pullups alone might be too weak. 

1wire bus:
- 4k7 Pullups
- Arduino default-Pinout: GND,VCC,DO

Original Rj11 jacks from the Davis-Sensors will fit to the terminals. 
Windsensor: 
- Pin 1: n/c
- Pin 2: 3.3 Volt (yellow wire)
- Pin 3: winddirection (potentiometer, green wire)
- Pin 4: ground/gnd (red wire)
- Pin 5: Windspeed (reedswitch, black wire)
- Pin 6: n/c

Rainsensor (wirecolor 3 and 4 might be swapped, this is no problem): 
- Pin 1: n/c
- Pin 2: n/c
- Pin 3: Windspeed (reedswitch, green/yellow wire)
- Pin 4: ground/gnd (red wire)
- Pin 5: n/c
- Pin 6: n/c

BME280 Sensor (use a rj45 jack and solder the bme280 onto it). CAT6-Cable with up to 2 (perhaps 3) meter length should work. Activate pullups on the board (J1 and J2)
- Pin 1,2,4,6 = Ground/gnd
- Pin 7,8 = VCC 3.3 Volt 
- Pin 2 = SCL
- Pin 5 = SDA

Jumper 1 is a simple flying lead. This was necessary because there was no longer any space available on the PCB for this connection.
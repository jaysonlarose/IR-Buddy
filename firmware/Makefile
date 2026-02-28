BOARD_TAG = leonardo
ARDUINO_PORT = /dev/ttyACM2

ARDUINO_LIBS = Arduino-IRremote2
USER_LIB_PATH = src

include /usr/share/arduino/Arduino.mk

defines:
	echo | $(CXX) -x c++ $(CPPFLAGS) $(CXXFLAGS) - -E -dM | sort

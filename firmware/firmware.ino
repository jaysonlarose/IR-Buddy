#include "src/Arduino-IRremote2/IRremote.h"
#include "src/Arduino-IRremote2/IRremoteInt.h"
#include <avr/pgmspace.h>

#undef IRRECV_PIN
#ifdef __AVR_ATmega644P__
#define IRRECV_PIN 12
#endif

#ifndef IRRECV_PIN
#define IRRECV_PIN 4
#endif

#ifdef IRRECV_PIN
decode_results results;
IRrecv irrecv(IRRECV_PIN, 13);
#endif

// In order to fit the longer durations into uint16_t, we divide incoming durations by this value in ASCII mode.
// In binary mode, all incoming code durations must be predivided by this value.
// Note that IR reception is further constrained by the USECPERTICK def, which comes out of the box as 50.
// In binary mode, the appropriate math is performed so that the values sent over the line conform to the CODE_DIV division and not USECPERTICK.
#define CODE_DIV 10
#define SETTLE_DURATION 5000

#define OP_IR_TRANSMIT  0x01
#define OP_IR_RECEIVE   0x02
#define OP_EXIT_BINARY  0x03
#define OP_ACK          0x04
#define OP_NAK          0x05
#define OP_REPEATING    0x06
#define OP_ENTER_BINARY 0xff

int code_mult_binary;

IRsend irsend;


// binary_mode is set to 1 when '\xff' is received, which puts the logic into binary protocol mode.
uint8_t ansi_mode = 1;
uint8_t binary_mode = 0;
uint8_t binary_mux = 0;
uint8_t binary_freq = 0;
uint16_t binary_codelen = 0;
uint16_t binary_repeatlen = 0;
// codepos stores the length of entries in codebuf that comprise the primary code.
uint16_t codepos = 0;
bool printed_rx = false;
// charbuf stores incoming ASCII characters from the UART.
// As parsing occurs in realtime, no more than 8 bytes are needed.
char charbuf[8];
uint8_t charpos = 0;

// non-repeating code (repeatstate == 0):
// 0 -> codepos
//
// simple repeat (repeatstate == 1):
// 0 -> codepos -> repeatpos

uint8_t repeatstate = 0;
uint16_t repeatpos = 0;

// Emitter carrier frequency in kHz
int emitfreq = 38;
int current_emitfreq = 0;

extern volatile irparams_t irparams;

#ifdef IRRECV_PIN
// When a full incoming IR transmission has been received, IR-buddy
// will spit it out like so:
// IR ONDUR OFFDUR ONDUR OFFDUR ETC ETC ONDUR\n
// "IR" is the literal ascii characters "IR".
// ONDUR and OFFDUR come in pairs, corresponding to the amount of
// time the intercepted pulse was ON and OFF, in microseconds.
// End of reception is indicated with a newline.  Easy peasy.
void dump(decode_results *results) {
	Serial.print(F("IR"));
	for (uint16_t i = 0; i < results->rawlen; i++) {
		Serial.print(' ');
		//Serial.print(results->rawbuf[i] * USECPERTICK);
		Serial.print((unsigned long) results->rawbuf[i] * USECPERTICK);
	}
	Serial.println("");
}

void dump_binarymode(decode_results *results) {
	// send opcode
	Serial.write(OP_IR_RECEIVE);
	// send results length
	Serial.write((uint8_t) (results->rawlen >> 8));
	Serial.write((uint8_t) (results->rawlen & 0xff));
	// send code timings
	for (uint16_t i = 0; i < results->rawlen; i++) {
		//// Multiply to convert from USCEPERTICK division to CODE_DIV division
		uint16_t val_binary = results->rawbuf[i] * code_mult_binary;
		// Send her down the line big-endian stylee.
		Serial.write((uint8_t) (val_binary >> 8));
		Serial.write((uint8_t) (val_binary & 0xff));
	}
}
		
#endif

const uint8_t PROGMEM irmux_to_pin_PGM[] = { 5, 6, 7, 8, 13, 14, 15, 16 };

void set_irmux(uint8_t mux) {
	for (uint8_t i = 0; i < 8; i++) {
		uint8_t muxpin = pgm_read_byte(irmux_to_pin_PGM + i);
		if (muxpin != 0) {
			if ((mux & (1 << i)) > 0) {
				digitalWrite(muxpin, HIGH);
			} else {
				digitalWrite(muxpin, LOW);
			}
		}
	}
}

void setup() {
	Serial.begin(115200);
	// Set multiplier to use in binary mode to convert Arduino-IRremote's
	// raw USECPERTICK format to my CODE_DIV format.
	code_mult_binary = USECPERTICK / CODE_DIV;
	// make rawbuf_a the active buffer.
	irparams.rawbuf = irparams.rawbuf_a;
	// set mux pin direction to output
	for (uint8_t i = 0; i < 8; i++) {
		uint8_t muxpin = pgm_read_byte(irmux_to_pin_PGM + i);
		if (muxpin != 0) {
			pinMode(muxpin, OUTPUT);
		}
	}
	// initial mux
	set_irmux(0xff);
	#ifdef IRRECV_PIN
	// set the receive pin direction to INPUT
	pinMode(IRRECV_PIN, INPUT);
	// start the receiver
	irrecv.enableIRIn();
	#endif
}

// Reimplementation of IRsend::mark, because the Arduino-IRremote version is
// constrained to unsigned int.
void mark_long(IRsend sender, unsigned long time) {
	TIMER_ENABLE_PWM;
	if (time > 0) {
		sender.custom_delay_usec(time);
	}
}

// Reimplementation of IRsend::space, because the Arduino-IRremote version is
// constrained to unsigned int.
void space_long(IRsend sender, unsigned long time) {
	TIMER_DISABLE_PWM;
	if (time > 0) {
		sender.custom_delay_usec(time);
	}
}

// Reimplementation of IRsend::sendraw, which multiplies all code durations by
// a supplied factor.
void sendraw_mult(IRsend sender, volatile unsigned int buf[], unsigned int len, unsigned int multiplier) {
	//Serial.println("SENDING");

	for (unsigned int i = 0; i < len; i++) {
		unsigned long duration = (unsigned long) buf[i] * multiplier;
		if (i & 1) {
			space_long(sender, duration);
		} else {
			mark_long(sender, duration);
		}
	}

	space_long(sender, 0);
}

// Pulled rawval out into a global variable so that we can persist
// the value around in case we need to use it as a frequency specifier.
unsigned long rawval = 0;


// IR emit format
// ON + ' ' + OFF + ' ' + ON + ' ' + OFF + ' ' + etc etc + ON + '\r\n'
// ON and OFF are strings of ascii characters '0'-'9' defining the
// mark durations and space durations, in microseconds.

//
// [ON + ' ' + OFF + ' ' + ON + ' ' + OFF + ' ' + etc etc + ON] + OFF1 + ' R ' + [ON + ' ' + OFF + ' ' + etc etc + OFF2] + '\r\n'
// Send the code between the first set of brackets
// delay OFF1 microseconds
// then continually repeat the code between the second set of brackets until an additional character is received on serial.

// on off on off on off on off r on off on off
// codepos = 8
// repeatpos = 4


// Binary protocol
// The host sends 0xff to initiate binary protocol.
// Sending an IR transmission request:
// 0x01 (Start of Heading) + 8-bit frequency + 8-bit mux + 8-bit codelen + 8-bit repeatlen + 0x02 (Start of Text) + (series of 16-bit big-endian code durations*) + (series of 16-bit repeat durations*) + 0x03 (End of Text)
// * All of these code durations should be divided by 10 before transmission.
// Leaving binary protocol:
// Keep sending 0x03 (CTRL+C on most terminal emulators) until you start getting ANSI back.

//int ram_avail() {
//	extern int __bss_end;
//	extern void *__brkval;
//	int free_memory;
//	if ((int) __brkval == 0) {
//		free_memory = ((int) &free_memory) - ((int) &__bss_end);
//	} else {
//		free_memory = ((int) &free_memory) - ((int) __brkval);
//	}
//	return free_memory;
//}

void parser_reset() {
	if (binary_mode) {
	} else {
		set_irmux(0xff);
		irrecv.enableIRIn();
		//#ifdef IRRECV_PIN
		//irrecv.enableIRIn();
		//#endif
		//Serial.print(F("RAM free: "));
		//Serial.println(ram_avail());
		codepos = 0;
		charpos = 0;
		repeatpos = 0;
		repeatstate = 0;
		emitfreq = 38;
		printed_rx = false;
		// cursor_horizontal_absolute(1) + save_cursor_position
		if (ansi_mode) {
			Serial.print(F("\x1b[1G\x1b[s"));
			Serial.flush();
		}
	}
}

uint8_t recv_8bit() {
	while (!Serial.available()) {
	}
	char c = Serial.read();
	return (uint8_t) c;
}

uint16_t recv_16bit() {
	uint16_t ret = 0;
	ret |= recv_8bit() << 8;
	ret |= recv_8bit();
	return ret;
}

	

void loop() {
	#ifdef IRRECV_PIN
	// Do not process IR events if we're receiving serial data.
	// This is done just to make automating the thing nice and
	// predictable:
	// If you want to send something, send something.
	// If there's a partial code in the receive buffer,
	// it'll be nuked. After the buddy has finished
	// flushing the serial buffer and sending the code,
	// IR receive will resume until the next time you
	// start to send serial data.
	// Note: When you start sending serial data, ir-buddy will
	// respond to the first character with "RX ", to let you know
	// that it sees you're sending it something, and that it's
	// no longer listening for IR. When you finish transmission by
	// issuing a newline, IR-buddy will acknowledge with either "OK\n"
	// or "ERROR".
	// Thus, if you read back "RX" and what you really want to do
	// is listen for IR, throw it a ^C (0x03) to cancel. IR-buddy
	// will confirm with "CANCEL\n".
	if (codepos == 0 && charpos == 0) {
		if (irrecv.decode(&results)) {
			if (binary_mode) {
				dump_binarymode(&results);
			} else {
				dump(&results);
			}
			irrecv.resume();
		}
	}
	#endif
	while (Serial.available()) {
		//char c = Serial.read();
		uint8_t c = Serial.read();
		//Serial.write(c);
		if (binary_mode) {
			if (c == OP_IR_TRANSMIT) {
				irrecv.pause();
				binary_mux = recv_8bit();
				binary_freq = recv_8bit();
				binary_codelen = recv_16bit();
				binary_repeatlen = recv_16bit();
				for (uint16_t i = 0; i < binary_codelen + binary_repeatlen; i++) {
					irparams.rawbuf[i] = recv_16bit();
				}
				set_irmux(binary_mux);
				if (binary_repeatlen > 0) {
					//if (irparams.rawbuf[binary_codelen - 1] >= SETTLE_DURATION / CODE_DIV) {
					//	irparams.rawbuf[binary_codelen - 1] -= (SETTLE_DURATION / CODE_DIV);
					//}
					//if (irparams.rawbuf[binary_codelen + binary_repeatlen - 1] >= SETTLE_DURATION / CODE_DIV) {
					//	irparams.rawbuf[binary_codelen - binary_repeatlen - 1] -= (SETTLE_DURATION / CODE_DIV);
					//}
					// send OP_REPEATING
					Serial.write(OP_REPEATING);
				} else {
					// send OP_ACK
					Serial.write(OP_ACK);
				}
				irsend.enableIROut(binary_freq);
				space_long(irsend, SETTLE_DURATION);
				sendraw_mult(irsend, irparams.rawbuf, binary_codelen, CODE_DIV);
				if (binary_repeatlen > 0) {
					while (!Serial.available()) {
						sendraw_mult(irsend, irparams.rawbuf + binary_codelen, binary_repeatlen, CODE_DIV);
					}
					Serial.read();
					// send OP_ACK
					Serial.write(OP_ACK);
				}
				irrecv.enableIRIn();
					
					
			} else if (c == OP_ENTER_BINARY) {
				Serial.write(OP_ACK);
			} else if (c == OP_EXIT_BINARY) {
				binary_mode = 0;
			}
			parser_reset();
		} else {
			if (c == OP_ENTER_BINARY) {
				binary_mode = 1;
				Serial.write(OP_ACK);
			// CTRL+C and ESC cancel input
			} else if (c == 0x03 || c == 0x1b) {
				// restore_cursor_position + erase_line_from_cursor + "CANCEL"
				if (ansi_mode) {
					Serial.print(F("\x1b[u\x1b[0K"));
				}
				Serial.println(F("CANCEL"));
				parser_reset();
			// Digits get fed into charbuf, where they'll get parsed into
			// binary values when ' ' or '\n' is encountered.
			} else if (c >= '0' && c <= '9') {
				if (codepos == 0 && charpos == 0 && !printed_rx) {
					Serial.print(F("RX "));
					irrecv.pause();
					if (ansi_mode) {
						// save_cursor_position
						Serial.print(F("\x1b[s"));
					}
					printed_rx = true;
				}
				if (ansi_mode && charpos == 0) {
					// restore_cursor_position
					Serial.print(F("\x1b[u"));
					Serial.print(codepos + repeatpos + 1);
				}
				charbuf[charpos] = c;
				if (charpos < 8) {
					charpos++;
				}
			// 'A' toggles between ANSI mode and ASCII mode
			} else if (c == 'A') {
				ansi_mode ^= 1;
				if (ansi_mode) {
					Serial.println(F("ANSI"));
				} else {
					Serial.println(F("ASCII"));
				}
				parser_reset();
			// 'R' deliniates the main body of an IR code from its repeat value
			} else if ((c == 'R') && charpos == 0) {
				repeatstate = 1;
			// 'F' specifies the previous value is to be used to set the PWM frequency
			} else if ((c == 'F') && charpos == 0 && repeatstate == 0 && codepos > 0) {
				// pull emitfreq from the persisted rawval
				emitfreq = rawval;
				// rewind codepos
				codepos--;
				// restore_cursor_position + "FREQ " + save_cursor_position
				if (ansi_mode) {
					Serial.print(F("\x1b[uFREQ \x1b[s"));
					Serial.print(codepos + repeatpos);
				}
			// 'M' specifies the previous value is to be used to set the muxing
			} else if ((c == 'M') && charpos == 0 && repeatstate == 0 && codepos > 0) {
				// here
				if (rawval > 255) {
					// restore_cursor_position + erase_line_from_cursor
					if (ansi_mode) {
						Serial.print(F("\x1b[u\x1b[0K"));
					}
					Serial.println(F("ERROR"));
					parser_reset();
				} else {
					uint8_t muxval = (uint8_t) rawval;
					set_irmux(muxval);
					codepos--;
					// restore_cursor_position + "MUX " + save_cursor_position
					if (ansi_mode) {
						Serial.print(F("\x1b[uMUX \x1b[s"));
						Serial.print(codepos + repeatpos);
					}
				}
			// if charbuf has data in it, ' ' or '\n' prompt digestion of charbuf.
			} else if ((c == ' ' || c == '\n') && charpos > 0) {
				charbuf[charpos] = '\0';
				charpos = 0;
				rawval = strtoul(charbuf, NULL, 10);
				irparams.rawbuf[codepos + repeatpos] = (unsigned int) (rawval / CODE_DIV);
				if (repeatstate > 0) {
					repeatpos++;
				} else {
					codepos++;
				}
				// restore_cursor_position
				//Serial.print("\x1b[u");
				//Serial.print(codepos + repeatpos);
			// otherwise, extra spaces are a no-op
			} else if (c == ' ') {
			} else if (c == '\n') {
			// completely ignore carriage return.
			} else if (c == '\r') {
			} else {
				if (ansi_mode) {
					// restore_cursor_position + erase_line_from_cursor
					Serial.print(F("\x1b[u\x1b[0K"));
				}
				Serial.println(F("ERROR"));
				parser_reset();
			}

			// note this isn't an "else if"
			// '\n' finishes input and prompts action.
			if (c == '\n' && codepos > 0) {
				//Serial.println();
				//Serial.println(codepos);
				if (ansi_mode) {
					// restore_cursor_position + erase_line_from_cursor
					Serial.print(F("\x1b[u\x1b[0K"));
				}
				if (codepos + repeatpos == RAWBUF) {
					Serial.println(F("ERROR"));
				} else if (repeatpos > 0) {
					// This needs to be "% 2 == 0" because there's an off duration at the end.
					if (codepos % 2 != 0 || repeatpos % 2 != 0) {
						Serial.println(F("ERROR"));
					} else {
						//if (irparams.rawbuf[codepos - 1] >= SETTLE_DURATION / CODE_DIV) {
						//	irparams.rawbuf[codepos - 1] -= (SETTLE_DURATION / CODE_DIV);
						//}
						//if (irparams.rawbuf[codepos + repeatpos - 1] >= SETTLE_DURATION / CODE_DIV) {
						//	irparams.rawbuf[codepos + repeatpos - 1] -= (SETTLE_DURATION / CODE_DIV);
						//}
						Serial.println(F("REPEAT"));
						Serial.flush();
						irsend.enableIROut(emitfreq);
						space_long(irsend, SETTLE_DURATION);
						sendraw_mult(irsend, irparams.rawbuf, codepos, CODE_DIV);
						while (!Serial.available()) {
							sendraw_mult(irsend, irparams.rawbuf + codepos, repeatpos, CODE_DIV);
						}
						Serial.read();
						Serial.println(F("OK"));
							
					}
				} else {
					Serial.println(F("OK"));
					
					Serial.flush();
					irsend.enableIROut(emitfreq);
					space_long(irsend, SETTLE_DURATION);
					sendraw_mult(irsend, irparams.rawbuf, codepos, CODE_DIV);
				}
				parser_reset();
			}
		}
	}
}

#include "IRremote.h"
#include "IRremoteInt.h"

//+=============================================================================
// Decodes the received IR message
// Returns 0 if no data ready, 1 if data ready.
// Results of decoding are stored in results
//
int  IRrecv::decode (decode_results *results)
{
	results->overflow = irparams.overflow;

	if (irparams.rcvstate != STATE_STOP)  return false ;
	if (irparams.rawlen == 0) return false;

	results->rawbuf   = irparams.rawbuf;
	results->rawlen   = irparams.rawlen;

	irparams.rawlen = 0;
	// Switch active rawbuf.
	if (irparams.rawbuf == irparams.rawbuf_a) {
		irparams.rawbuf = irparams.rawbuf_b;
	} else {
		irparams.rawbuf = irparams.rawbuf_a;
	}
	return true;
}

//+=============================================================================
IRrecv::IRrecv (int recvpin)
{
	irparams.recvpin = recvpin;
	irparams.blinkflag = 0;
}

IRrecv::IRrecv (int recvpin, int blinkpin)
{
	irparams.recvpin = recvpin;
	irparams.blinkpin = blinkpin;
	pinMode(blinkpin, OUTPUT);
	irparams.blinkflag = 0;
}



//+=============================================================================
// initialization
//
void  IRrecv::enableIRIn ( )
{
	cli();
	// Setup pulse clock timer interrupt
	// Prescale /8 (16M/8 = 0.5 microseconds per tick)
	// Therefore, the timer interval can range from 0.5 to 128 microseconds
	// Depending on the reset value (255 to 0)
	TIMER_CONFIG_NORMAL();

	// Timer2 Overflow Interrupt Enable
	TIMER_ENABLE_INTR;

	TIMER_RESET;

	sei();  // enable interrupts

	// Initialize state machine variables
	irparams.rcvstate = STATE_IDLE;
	irparams.rawlen = 0;

	// Set pin modes
	pinMode(irparams.recvpin, INPUT);
}

//+=============================================================================
// Enable/disable blinking of pin 13 on IR processing
//
void  IRrecv::blink13 (int blinkflag)
{
	irparams.blinkflag = blinkflag;
	if (blinkflag)  pinMode(BLINKLED, OUTPUT) ;
}

//+=============================================================================
// Return if receiving new IR signals
//
bool  IRrecv::isIdle ( )
{
 return (irparams.rcvstate == STATE_IDLE || irparams.rcvstate == STATE_STOP) ? true : false;
}
//+=============================================================================
// Restart the ISR state machine
//
void  IRrecv::resume ( )
{
	irparams.rcvstate = STATE_IDLE;
	irparams.rawlen = 0;
}

void IRrecv::pause ( )
{
	irparams.rcvstate = STATE_STOP;
	irparams.rawlen = 0;
}


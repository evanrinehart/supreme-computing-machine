#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* audio processing unit */

// the apu contains several oscillators which are controlled by
// precisely timed register writes by the cpu. So we keep an event queue
// of what to do when.

struct APUEvent {
    unsigned char action;
    unsigned char data;
};

#define EVENT_QUEUE_SIZE 256
struct APUEvent eventQueue[EVENT_QUEUE_SIZE];
float eventQueueTimes[EVENT_QUEUE_SIZE];
int eventQueueBase = 0;
int eventQueuePtr = 0;
int eventQueueAmount = 0;

void insertAudioEvent(struct APUEvent e, float time){
    if(EVENT_QUEUE_SIZE - eventQueueAmount == 0){
        printf("audio event queue overflow :(\n");
        return;
    }

    eventQueue[eventQueuePtr] = e;
    eventQueueTimes[eventQueuePtr] = time;
    eventQueuePtr++;
    if(eventQueuePtr == EVENT_QUEUE_SIZE) eventQueuePtr = 0;
    eventQueueAmount++;
}

void dequeueAudioEvent(){
    if(eventQueueAmount == 0){
        fprintf(stderr, "dequeueAudioEvent: empty queue, your logic leaves much to be desired\n");
        exit(1);
    }
    eventQueueBase++;
    eventQueueAmount--;
}

int peekAudioEvent(struct APUEvent *e, float *time){
    if(eventQueueAmount == 0) return 0;
    *e    = eventQueue[eventQueueBase];
    *time = eventQueueTimes[eventQueueBase];
    return 1;
} 


struct SquareWave {
    float phase;
    float dt;
    float volume;
    int length; // decreases over time, silence note if it reaches zero
    unsigned char timerHigh;
    unsigned char timerLow;
    unsigned char duty;//0=12.5% 1=25% 2=50% 3=25% negated
    unsigned char loop; // if 0, stop when length counter reaches zero
    unsigned char constant; // if 0, decreasing envelope will be used for volume
};

struct SquareWave sqr[2] =
    {{0.0, 220.0/44100.0, 0.0, 7, 255, 2},
     {0.0, 220.0/44100.0, 0.0, 7, 255, 1}};

// 0 to 1, repeats,
float phase = 0.0;
// phase increment between samples, 1.0 represents oscillation equal to sample rate
float dt = 220.0 / 44100.0;
// simply silences the square wave if 1
int enable = 0;

float polyblep(float dt, float t){
    if(t < dt){
        t /= dt;
        return t+t - t*t - 1.0;
    }
    else if(1.0 - dt < t){
        t = (t - 1.0) / dt;
        return t*t + t+t + 1.0;
    }
    else
        return 0.0;
}

float squareWave(float dt, float t){
    float value = t < 0.5 ? 1.0 : -1.0;
    value += polyblep(dt, t);
    value -= polyblep(dt, fmod(t + 0.5, 1.0));
    return value;
}

float sqrGenerator(struct SquareWave *g){
    float out = 0.0;
    if(g->volume == 0) return 0.0;
    if(g->length == 0) return 0.0;
    if(g->duty == 2){ // 50% duty cycle
        out += 0.1 * g->volume * squareWave(g->dt, g->phase);
        g->phase += g->dt;
        if(g->phase > 1.0) g->phase -= 1.0;
    }
    else{
        float shift = g->duty == 0 ? 0.0625 : 0.125; // 12.5% or 25%
        out += squareWave(g->dt/2, g->phase);
        out -= squareWave(g->dt/2, fmod(g->phase + shift, 1));
        out /= 2.0;
        out = fabs(out);
        out -= 0.5;
        out *= 0.1 * g->volume;
        //out = fabs(out);
        //out -= ;
        g->phase += g->dt/2;
        if(g->phase > 1.0) g->phase -= 1.0;
    }
    return out;
}

void setTimerLow(int ch, unsigned char byte){
    sqr[ch].timerLow = byte;
    int period = (sqr[ch].timerHigh << 8) | sqr[ch].timerLow;
    float f = 1789773.0 / (16.0 * (period + 1));
    sqr[ch].dt = f / 44100.0;
}

void setTimerHigh(int ch, unsigned char byte){
    sqr[ch].timerHigh = byte;
    int period = (sqr[ch].timerHigh << 8) | sqr[ch].timerLow;
    float f = 1789773.0 / (16.0 * (period + 1));
    sqr[ch].dt = f / 44100.0;
}

void setVolume(int ch, unsigned char vol){
    float amplitude = (1.0 / 16.0) * (float)vol;
    sqr[ch].volume = amplitude;
}

void setDutyCycle(int ch, unsigned char d){
    sqr[ch].duty = d;
}

void setLoop(int ch, unsigned char l){
    sqr[ch].loop = l;
}

void setConstantVolume(int ch, unsigned char c){
    sqr[ch].constant = c;
}

unsigned char length_table[32] =
    {
        10, 254, 20, 2, 40, 4, 80, 6,
        160, 8, 60, 10, 14, 12, 26, 14,
        12, 16, 24, 18, 48, 20, 96, 22,
        192, 24, 72, 26, 16, 28, 32, 30
    };

void setLengthCounter(int ch, unsigned char n){
    sqr[ch].length = length_table[n];
}

// this frame counter has nothing to do with video frames
int frameCounter = 2*14915;
int frameCounterEnable = 0;
int frameCounterPeriod = 2*14915;
int frameCounterMode = 0;
//int frameCounter = 18641;
void apuFrameHalfClock(){
    if(frameCounterEnable == 0) return;

    frameCounter++;

    int wholeFrame = frameCounter / 2;
    int halfFrame = frameCounter % 2;

    if(wholeFrame == 3728 && halfFrame){
        // envelope, linear counter clock
    }

    if(wholeFrame == 7456 && halfFrame){
        // envelope, linear counter clock
        // length counter, sweep units

        if(sqr[0].length > 0 && sqr[0].loop == 0){ sqr[0].length--; }
        if(sqr[1].length > 0 && sqr[1].loop == 0){ sqr[1].length--; }
    }

    if(wholeFrame == 11185 && halfFrame){
        // envelope, linear counter clock
    }

    if(
        (frameCounterMode == 0 && wholeFrame == 14914 && halfFrame) ||
        (frameCounterMode == 1 && wholeFrame == 18640 && halfFrame)
    )
    {
        // envelope, linear counter clock
        // length counter, sweep units
        if(sqr[0].length > 0 && sqr[0].loop == 0){ sqr[0].length--; }
        if(sqr[1].length > 0 && sqr[1].loop == 0){ sqr[1].length--; }
    }

    if(frameCounter == frameCounterPeriod) frameCounter = 0;
}

void setFrameCounterEnable(int en){
    frameCounterEnable = en;
    if(en == 0){
        sqr[0].length = 0;
        sqr[1].length = 0;
    }
}

void setFrameCounterPeriod(unsigned char bit){
    if(bit == 0) frameCounterPeriod = 14915;
    if(bit == 1) frameCounterPeriod = 18641;
}





/* programmable timer
this device counts down from N+1 to zero then resets.
N is configured in the control register for the relevant channel
since it's driven by the CPU clock, the frequency is 1.79MHz / (N+1)
it is used to drive the waveform generators, and so their frequency.
*/

/* length counter
a 60Hz counter which counts down from some value to zero.
used to stop waveform generators when a note ends.
can be disabled.
*/

/* 4-bit DAC
4 input bits determine an output voltage, which goes to the mixers.
The inputs can come directly from a generator (triangle) or be gated
by an envelope generator (square, noise). The output is one of 16 values,
but depending on signal amplitude we will have varying DC bias which
needs to be dynamically removed before output.
*/

/* volume / envelope decay unit
has 2 modes. mode 1: outputs a constant 4-bit volume level
mode 2: a 4-bit counter decreases at a configurable rate, decreasing
the volume level each time until zero. Can be configured to loop back
to 1111 or remain at zero.
*/

/* sweep unit
a counter which, if enabled, increases or decreases and updates some
frequency register to make a sweep effect. It stops when the connected
length counter reaches zero or sweep increases to max (carry detected).
if sweep increases to max the channel is silenced.
*/

/* sequencer
outputs a sequence of bits as it clocks, the sequence depends on one
of 4 options. This is used to gate the envelope signal in the pulse
wave generators.
*/

/*

output = snd0 + snd1 // actually a more complex formula
snd0 = square0 + square1
square0 = gate(sequencer0, v1)
v1 = gate(sweep0silence, envelope0)
envelope = depends on mode, either constant volume or current level
sequencer0 = dutyCycle[x][i]   i increments when timer0 goes from 0 to N+1
timer0 = a counter with period (N + 1) / 1.79MHz



triangle equations
output = sequencer(v1, {0,1,2,3,...F,F,E,D,C,...0}) :: V
v1 = gate(v2, lenc) :: E
v2 = gate(v3, linc) :: E
v3 = timer(clock1, N+1) :: E
lenc = counter(L) :: V
linc = counter(L) :: V

events - occur at a point in time, or periodically
values/voltages - hold a value over a period of time and can be sampled

clock1 - the 1.79MHz CPU clock, an event
apuClk - 

*/


struct TriangleGenerator {
    unsigned char counter; // 5 bits
    unsigned char output; // 4 bits
    // input1: length counter OR linear counter == 0
    // input2: programmable timer terminal count
};

struct TriangleGenerator tri = {0,0};

// happens when programmer timer reaches 0
// unless length counter OR linear counter are zero
void clockTriangleGenerator(struct TriangleGenerator *g){
    g->counter = (g->counter + 1) & 0x1f;
    g->output = ((g->counter >> 4) ? 0xf : 0x0) ^ (g->counter & 0xf);
}

void test(){
    for(int i = 0; i < 50; i++){
        printf("counter = %02x, output = %02x\n", tri.counter, tri.output);
        clockTriangleGenerator(&tri);
    }
}




// generate numSamples more samples worth of output
// each sample is 1/44100 seconds of time
// events might occur between samples
// in 1/44100 seconds, the CPU cycles 40.4595 times
void synth(float *out, int numSamples){

    if(!sqr[0].volume && !sqr[1].volume){
        for(int i = 0; i < numSamples; i++){
            out[i] = 0.0;
        }
        return;
    }

    for(int i = 0; i < numSamples; i++){
        out[i] = 0.0;

        out[i] += sqrGenerator(&sqr[0]);
        out[i] += sqrGenerator(&sqr[1]);
    }

}


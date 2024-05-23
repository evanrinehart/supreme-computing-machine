#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>

#include <threads.h>

#include <raylib.h>

#include <rom.h>
#include <instructions.h>
#include <colors.h>

extern void test(void);
extern void setEnable(int ch, unsigned char en);
extern void setVolume(int ch, unsigned char vol);
extern void setDutyCycle(int ch, unsigned char d);
extern void setEnvelope(int ch, unsigned char byte);
extern void setSweep(int ch, unsigned char byte);
extern void setLengthCounter(int ch, unsigned char n);
//extern void setFrequency(int ch, float f);
extern void setTimerLow(int ch, unsigned char byte);
extern void setTimerHigh(int ch, unsigned char byte);
extern void synth(float *out, int numSamples);
extern void apuFrameHalfClock();
extern void setFrameCounterPeriod(unsigned char bit);

unsigned char memory[65536];

int timeDilation = 1;
int timeFreeze = 0;

int dmaFlag = 0;

int frameNo = 0;
int scanline = 0;
int dot = 0;

#define WRITELOG_SIZE 64
int writeLog[WRITELOG_SIZE];
int writeLogPtr = 0;

int pcLog[8] = {0,0,0,0,0,0,0,0};
int pcLogPtr = 0;

void remember(int pc){
    pcLog[pcLogPtr] = pc;
    pcLogPtr++;
    if(pcLogPtr == 8) pcLogPtr = 0;
}

void showPcLog(){
    for(int i = 0; i < 8; i++){
        printf("i=%d pc=%04x %s\n", i, pcLog[i], pcLogPtr==i ? "*" : "");
    }
}

void logWrite(int addr){
    if(writeLogPtr == WRITELOG_SIZE){
        for(int i = 0; i < WRITELOG_SIZE - 1; i++){
            writeLog[i] = writeLog[i+1];
        }
        writeLog[WRITELOG_SIZE-1] = addr;
    }
    else{
        writeLog[writeLogPtr++] = addr;
    }
}

struct InterruptVectors {
    int nmi;
    int reset;
    int irq;
};

struct InterruptVectors vectors;

struct ProcessorStatus {
    int carry;
    int zero;
    int interruptDisable;
    int decimal;
    int overflow;
    int negative;
};

unsigned char packProcessorStatus(struct ProcessorStatus p) {
    unsigned char byte = 0;
    byte |= p.negative << 7;
    byte |= p.overflow << 6;
    byte |= (1 << 5);
    // bit 4 is zero when NMI causes p to be packed and pushed
    // bit 4 is one  when BRK causes p to be packed and pushed
    byte |= p.decimal << 3;
    byte |= p.interruptDisable << 2;
    byte |= p.zero << 1;
    byte |= p.carry;
    return byte;
}

struct ProcessorStatus unpackProcessorStatus(unsigned char byte){
    struct ProcessorStatus p;
    p.negative = (byte >> 7) & 1;
    p.overflow = (byte >> 6) & 1;
    p.decimal = (byte >> 3) & 1;
    p.interruptDisable = (byte >> 2) & 1;
    p.zero = (byte >> 1) & 1;
    p.carry = byte & 1;
    return p;
}

struct OAMEntry {
    unsigned char topY;
    unsigned char tile;
    unsigned char vflip;
    unsigned char hflip;
    unsigned char priority;
    unsigned char palette;
    unsigned char leftX;
};

struct OAMEntry unpackOAMEntry(unsigned char *bytes){
    struct OAMEntry out;
    out.topY     = bytes[0];
    out.tile     = bytes[1];
    out.vflip    = (bytes[2] >> 7) & 1;
    out.hflip    = (bytes[2] >> 6) & 1;
    out.priority = (bytes[2] >> 5) & 1;
    out.palette  = bytes[2] & 0x03;
    out.leftX    = bytes[3];
    return out;
}

void printOAMEntry(struct OAMEntry entry){
    printf("OAMEntry {\n");
    printf("  leftX = %02x\n", entry.leftX);
    printf("  topY  = %02x\n", entry.topY);
    printf("  tile  = %02x\n", entry.tile);
    printf("  vflip = %02x\n", entry.vflip);
    printf("  hflip = %02x\n", entry.hflip);
    printf("  priority = %02x\n", entry.priority);
    printf("  palette  = %02x\n", entry.palette);
    printf("}\n");
}

struct Registers {
    unsigned char A;
    unsigned char X;
    unsigned char Y;
    unsigned char S;
    int PC;
    struct ProcessorStatus P;
};

struct Registers regs = {0,0,0,0xfd,0,{0,0,1,0,0,0}};

struct NESHeader {
    unsigned char start[4];
    unsigned char prg_rom_size;
    unsigned char chr_rom_size;
    unsigned char flags6;
    unsigned char flags7;
    unsigned char flags8;
    unsigned char flags9;
    unsigned char flags10;
    unsigned char unused[5];
};

int prgbase = 0x8000;
unsigned char *prg;
unsigned char *chr;

int screenW = 320;
int screenH = 240;
int screenScale = 3;
Image screenImg;
Texture2D screenTex;

// $0000 - $1fff the CHR ROM
// $2000 - $2fff vram nametables (subject to mirroring)
// $3000 - $3eff unused
// $3f00 - $3f1f palette RAM indexes
// $3f20 - $3fff mirrors
#define VRAM_SIZE 0x4000
#define VRAM_MAX  0x3fff
unsigned char ppuMemory[VRAM_SIZE];
unsigned char oam[256]; // 64 x 4 bytes
int ppuAddr = 0; // also used for ppuV, internal scroll position
int oamAddr = 0;
int ppuT = 0; // internal coarse-x scroll position
int ppuX = 0; // internal fine-x scroll position
int ppuW = 0; // write toggle
int ppuScrollX = 0;
int ppuScrollY = 0;
int ppuNameBase = 0x2000;
int ppuFineX = 0;
unsigned char ppuDataReadBuffer = 0;

struct OAMEntry spriteOutputUnit[8];
int numSprites = 0;

struct PPUCtrl {
    int nametableBase;
    int vramAddressIncrement; // 0->1, 1->32
    int spritePatternAddress;
    int bgPatternAddress;
    int spriteSize;
    int extMaster;
    int nmiOutput;
};

struct PPUMask {
    int emphasisB;
    int emphasisG;
    int emphasisR;
    int showSprites;
    int showBackground;
    int showSpritesLeft;
    int showBackgroundLeft;
    int grayscale;
};

struct PPUStatus {
    int spriteOverflow;
    int spriteZeroHit;
    int inVblank;
};

unsigned char ppuCtrlByte;
struct PPUCtrl ppuCtrl = {0,0,0,0,0,0,0};
struct PPUMask ppuMask = {0,0,0,0,0,0,0,0};
struct PPUStatus ppuStatus = {0,0,0};



int gamepadA = 0;
int gamepadB = 0;
int gamepadSelect = 0;
int gamepadStart = 0;
int gamepadUp = 0;
int gamepadDown = 0;
int gamepadLeft = 0;
int gamepadRight = 0;
unsigned char gamepadShiftRegister = 0;
void pollGamepad(){
    gamepadA = IsKeyDown(KEY_K);
    gamepadB = IsKeyDown(KEY_J);
    gamepadSelect = IsKeyDown(KEY_Q);
    gamepadStart = IsKeyDown(KEY_E);
    gamepadUp = IsKeyDown(KEY_W);
    gamepadDown = IsKeyDown(KEY_S);
    gamepadLeft = IsKeyDown(KEY_A);
    gamepadRight = IsKeyDown(KEY_D);
}

unsigned char packGamepad(){
    unsigned char byte;
    byte = 0;
    byte |= gamepadRight; byte <<= 1;
    byte |= gamepadLeft; byte <<= 1;
    byte |= gamepadDown; byte <<= 1;
    byte |= gamepadUp; byte <<= 1;
    byte |= gamepadStart; byte <<= 1;
    byte |= gamepadSelect; byte <<= 1;
    byte |= gamepadB; byte <<= 1;
    byte |= gamepadA;
    return byte;
}



void findSpritesOnLine(int line){
    // clear the spriteOutputUnits and load up to 8 on the given line
    numSprites = 0;
    for(int i = 0; i < 16; i++){
        unsigned char *ptr = &oam[i*4];
        int y = ptr[0];
        // this is ignoring priority
        if(line <= y && y <= line + 7){
            if(numSprites == 8) break; // sprite overflow?
            spriteOutputUnit[numSprites++] = unpackOAMEntry(ptr);
        }
    }
}


unsigned char read2002() {
    // reading PPU status register
    // has the effect of clearing bit 7
    // and resetting the internal w toggle

    unsigned char out = 0;

    out |= (ppuStatus.inVblank << 7);
    out |= (ppuStatus.spriteZeroHit << 6);
    out |= (ppuStatus.spriteOverflow << 5);
    ppuStatus.inVblank = 0;
    ppuW = 0;

    return out;
}

void write2000(unsigned char byte) {
    ppuCtrlByte = byte;
    ppuCtrl.nmiOutput = (byte >> 7) & 1; // might have immediate effect
    ppuCtrl.extMaster = (byte >> 6) & 1;
    ppuCtrl.spriteSize = (byte >> 5) & 1; // 8x8 or 8x16
    ppuCtrl.bgPatternAddress = (byte >> 4) & 1; // 0000 or 1000
    ppuCtrl.spritePatternAddress = (byte >> 3) & 1; // 0000 or 1000
    ppuCtrl.vramAddressIncrement = (byte >> 2) & 1; // add 1 or add 32
    ppuCtrl.nametableBase = byte & 0x03; // 2000, 2400, 2800, or 2c00

    switch(ppuCtrl.nametableBase){
        case 0: ppuNameBase = 0x2000; break;
        case 1: ppuNameBase = 0x2400; break;
        case 2: ppuNameBase = 0x2800; break;
        case 3: ppuNameBase = 0x2c00; break;
    }
    //printf("NameBase set to %04x\n", ppuNameBase);
}

void write2001(unsigned char byte) {
    ppuMask.emphasisB = (byte >> 7) & 1;
    ppuMask.emphasisG = (byte >> 6) & 1;
    ppuMask.emphasisR = (byte >> 5) & 1;
    ppuMask.showSprites = (byte >> 4) & 1;
    ppuMask.showBackground = (byte >> 3) & 1;
    ppuMask.showSpritesLeft = (byte >> 2) & 1;
    ppuMask.showBackgroundLeft = (byte >> 1) & 1;
    ppuMask.grayscale = (byte >> 0) & 1;
}



void resetCPU(){
    regs.P.interruptDisable = 1;
    regs.PC = vectors.reset;
}

void printBits(int byte){
    printf("%c", (byte >> 0) & 1 ? '1' : '0');
    printf("%c", (byte >> 1) & 1 ? '1' : '0');
    printf("%c", (byte >> 2) & 1 ? '1' : '0');
    printf("%c", (byte >> 3) & 1 ? '1' : '0');
    printf("%c", (byte >> 4) & 1 ? '1' : '0');
    printf("%c", (byte >> 5) & 1 ? '1' : '0');
    printf("%c", (byte >> 6) & 1 ? '1' : '0');
    printf("%c", (byte >> 7) & 1 ? '1' : '0');
}

char * locationName(int addr){
    switch(addr){
        case 0x90cc: return "InitializeMemory";
        case 0x802e: return "InitializeMemory call in boot script";
        case 0x8220: return "MoveAllSpritesOffscreen";
        case 0x8049: return "MoveAllSpritesOffscreen call in boot script";
        case 0x8e19: return "InitializeNameTables";
        case 0x804c: return "InitializeNameTables call in boot script";
        case 0x8e2d: return "WriteNTAddr";
        case 0x8e2b: return "WriteNTAddr call in InitializeNameTables";
        case 0x8ee6: return "InitScroll";
        case 0x80ae: return "InitScroll call in NMI";
        case 0x8edd: return "UpdateScreen";
        case 0x80c6: return "UpdateScreen call in NMI";
        case 0x8eed: return "WritePPUReg1";
        case 0x8eac: return "WritePPUReg1 call in WriteBufferToScreen";
        case 0x8e26: return "WritePPUReg1 call in InitializeNametables";
        case 0x8057: return "WritePPUReg1 call in boot script";
        case 0xf2d0: return "SoundEngine";
        case 0x80e7: return "SoundEngine call in NMI";
        case 0x8e5c: return "ReadJoypads";
        case 0x80ea: return "ReadJoypads call in NMI";
        case 0x8e6a: return "ReadPortBits";
        case 0x8e69: return "ReadPortBits call in ReadJoypads";
        case 0x8182: return "PauseRoutine";
        case 0x80ed: return "PauseRoutine call in NMI";
        case 0x8f97: return "UpdateTopScore";
        case 0x80f0: return "UpdateTopScore call in NMI";
        case 0x8f9e: return "TopScoreCheck";
        case 0x8f9c: return "TopScoreCheck call in UpdateTopScore";
        //case 0x8212: return "MoveSpritesOffscreen";
        //case 0x8178: return "MoveSpritesOffscreen call in NMI";
        case 0x8212: return "OperModeExecutionTree";
        case 0x8178: return "OperModeExecutionTree call in NMI";
        case 0x8e04: return "JumpEngine";
        case 0x8218: return "JumpEngine call in OperModeExecutionTree";
        case 0x9c03: return "LoadAreaPointer";
        case 0x9c13: return "FindAreaPointer";
        case 0xb038: return "GetScreenPosition";
        case 0x9c22: return "GetAreaDataAddrs";
        case 0x9c09: return "GetAreaType";
        case 0x85f1: return "GetPlayerColors";
        case 0x8808: return "WriteGameText";
        case 0xbc30: return "GetSBNybbles";
        case 0x8f06: return "PrintStatusBarNumbers";
        case 0x8f11: return "OutputNumbers";
        case 0x92b0: return "AreaParserTaskHandler";
        case 0x92c8: return "AreaParserTasks";
        case 0x9508: return "ProcessAreaData";
        case 0x9595: return "DecodeAreaData";
        case 0x9be1: return "GetBlockBufferAddr";
        case 0x8223: return "MoveSpritesOffscreen";
        case 0x81c6: return "SpriteShuffler";
        case 0x90ed: return "GetAreaMusic";
        case 0x92aa: return "DoNothing1";
        case 0x92af: return "DoNothing2";
        case 0xbc36: return "UpdateNumber";
        case 0x8325: return "DrawMushroomIcon";
        case 0xaeea: return "GameCoreRoutine";
        case 0xb04a: return "GameRoutines";
        case 0xb329: return "PlayerMovementSubs";
        case 0x8231: return "TitleScreenMode";
        case 0x8245: return "GameMenuRoutine";
        case 0xb0e9: return "PlayerCtrlRoutine";
        case 0xb36d: return "FallingSub";
        case 0xb450: return "PlayerPhysicsSub";
        case 0xbf09: return "MovePlayerHorizontally";
        case 0x84c3: return "FloateyNumbersRoutine";
        case 0xebb2: return "DrawOneSpriteRow";
        case 0xf26d: return "DividePDiff";
        case 0xe3ec: return "BlockBufferColli_Side";
        case 0xeee9: return "PlayerGfxHandler";
        case 0xbe70: return "BlockObjectsCore";
        case 0xc047: return "EnemiesAndLoopsCore";
        case 0xe29c: return "BoundingBoxCore";
        case 0x858b: return "InitScreen";
        case 0x8567: return "ScreenRoutines";
        //case 0xdc82: return "Mystery Location 1, line 11918";
        //case 0x0008: return "SpriteShuffler";
        //case 0x0009: return "OperModeExecutionTree";
        // Newms
        default: return "????";
    }
}

struct Instruction * instructionFromOpcode(int opcode){
    struct Instruction *ptr = &instructions[0];
    for(int i = 0; i < 256; i++){
        if(ptr->opcode == opcode) return ptr;
        ptr++;
    }
    printf("unknown opcode (%02x)\n", opcode);
    exit(1);
}

void printInstruction(int addr){
    int opcode = memory[addr];
    struct Instruction * ins = instructionFromOpcode(opcode);
    printf("%s", ins->mnemonic);
    if(ins->size == 3) printf(" %02x %02x", memory[addr+1], memory[addr+2]);
    if(ins->size == 2) printf(" %02x", memory[addr+1]);
    printf("\n");
}

void debug(){
    int opcode = memory[regs.PC];
    struct Instruction * ins = instructionFromOpcode(opcode);
    printf(
        "PC=%04x %s(%d) A=%02x Y=%02x X=%02x S=%02x [",
        regs.PC, ins->mnemonic, ins->size, regs.A, regs.Y, regs.X, regs.S
    );
    for(int i = regs.S+1; i < 256; i++){
        printf("%02x,", memory[0x0100 + i]);
    }
    struct ProcessorStatus p = regs.P;
    printf(
        "] (c%d z%d i%d d%d o%d n%d))\n",
        p.carry, p.zero, p.interruptDisable, p.decimal, p.overflow, p.negative
    );

    //showPcLog();
}

void showCPU(){
    printf("A = $%02x\n", regs.A);
    printf("X = $%02x\n", regs.X);
    printf("Y = $%02x\n", regs.Y);
    printf("S = $%02x\n", regs.S);
    printf("PC = $%04x\n", regs.PC);
    printf("carry = %d\n", regs.P.carry);
    printf("zero = %d\n", regs.P.zero);
    printf("interruptDisable = %d\n", regs.P.interruptDisable);
    printf("decimal = %d\n", regs.P.decimal);
    printf("overflow = %d\n", regs.P.overflow);
    printf("negative = %d\n", regs.P.negative);
    printf("instruction @ PC:\n");
    printInstruction(regs.PC);
    printf("\n");
}

#define UNCOMPLEMENT(X) ((X) < 128 ? (X) : (X - 256))

struct Instruction * fetchInstruction(int addr, int * arg1, int * arg2){
    int opcode = memory[addr];
    struct Instruction * ins = instructionFromOpcode(opcode);
    if(ins->size > 1) *arg1 = memory[addr+1];
    if(ins->size > 2) *arg2 = memory[addr+2];
    return ins;
}

unsigned char readMemory(int addr){

    unsigned char byte;

    if(addr == 0x2000 || addr == 0x2001 || addr == 0x2003 || addr == 0x2005 || addr == 0x2006){
        printf("WEIRD read from $%04x (normally write only)\n", addr);
        return 0;
    }
    else if(addr == 0x2002){
        //printf("read from $%04x (PPU status)\n", addr);
        return read2002();
    }
    else if(addr == 0x2004){
        //printf("read from $2004 (OAM data)\n");
        return oam[oamAddr];
    }
    else if(addr == 0x2007){
        byte = ppuDataReadBuffer;
//        if(ppuAddr == 0x3f10)
//            ppuDataReadBuffer = ppuMemory[0x3f00];
//        else
            ppuDataReadBuffer = ppuMemory[ppuAddr];
        //printf("read %02x (%02x) from $2007 (PPU data) (ppuAddr=%04x\n", ppuDataReadBuffer, byte, ppuAddr);

        if(ppuCtrl.vramAddressIncrement)
            ppuAddr = (ppuAddr + 32) & VRAM_MAX;
        else
            ppuAddr = (ppuAddr + 1) & VRAM_MAX;
        return byte;
    }
    else if(addr >= 0x4000 && addr <= 0x4014){
        printf("read from $%04x\n", addr);
        exit(1);
    }
    else if(addr == 0x4015){
        //printf("read from $%04x (sound channels and IRQ status)\n", addr);
        return 0;
    }
    else if(addr == 0x4016){
        byte = gamepadShiftRegister & 1;
        gamepadShiftRegister >>= 1;
        //printf("read from $%04x (joystick 1 data)\n", addr);
        return byte;
    }
    else if(addr == 0x4017){
        //printf("read from $%04x (joystick 2 data)\n", addr);
        return 0;
    }
    else if(addr >= 0x4018 && addr <= 0x401f){
        //printf("read from $%04x (disabled functionality)\n", addr);
        return 0;
    }
    else return memory[addr];
}

void writeMemory(int addr, unsigned char byte){
    if(addr == 0x2000) {
        //printf("frame=%d line=%d dot=%d PC=%04x write %02x to %04x (PPU ctrl)\n", frameNo, scanline, dot, regs.PC, byte, addr);
        write2000(byte);
    }
    else if(addr == 0x2001){
        //printf("write %02x to %04x (PPU mask)\n", byte, addr);
        write2001(byte);
    }
    else if(addr == 0x2003){
        //printf("write %02x to $2003 (OAM addr)\n", byte);
        oamAddr = byte;
    }
    else if(addr == 0x2004){
        // it's probably best to ignore writes to this register unless in blanking
        //printf("write %02x to $2004 (OAM data)\n", byte);
        oam[oamAddr] = byte;
        oamAddr = (oamAddr + 1) & 0xff;
    }
    else if(addr == 0x2005){
        if(ppuW == 0){
            //printf("frame=%d line=%d dot=%d PC=%04x write %02x to $2005 (PPU scroll X)\n", frameNo, scanline, dot, regs.PC, byte);
            ppuScrollX = byte;
            ppuFineX = byte & 7;
            ppuW = !ppuW;
        }
        else if(ppuW == 1){
            //printf("write %02x to $2005 (PPU scroll Y)\n", byte);
            ppuScrollY = byte;
            // mario doesn't have vertial scrolling
            ppuW = !ppuW;
        }
    }
    else if(addr == 0x2006){
        //printf("frame=%d line=%d dot=%d PC=%04x write %02x to $2006 (PPU addr) ", frameNo, scanline, dot, regs.PC, byte);
        if(ppuW == 0){
            ppuAddr = (int)byte << 8;
            ppuW = !ppuW;

            // internally, first write to this address clobbers the nametable base
            ppuCtrl.nametableBase = (byte >> 2) & 3;
        }
        else if(ppuW == 1){
            ppuAddr |= byte;
            ppuW = !ppuW;
        }
        //printf("now ppuAddr = %04x\n", ppuAddr);
    }
    else if(addr == 0x2007){
        //printf("write %02x to $2007 (PPU data) (addr=%04x)\n", byte, ppuAddr);
        if(ppuAddr < 0x2000){
            printf("PC=%04x WUT attempting to write to CHR ROM.\n", regs.PC);
            debug();
            printf("ppuAddr = %04x\n", ppuAddr);
            printf("data = %02x\n", byte);
            exit(1);
        }
        else if(ppuAddr > 0x3fff){
            printf("PPUDATA write out of range\n");
            exit(1);
        }
        else{
            if(ppuAddr >= 0x2800 && ppuAddr <= 0x2fff){
                printf("they tried to use mirroring (%04x)\n", ppuAddr);
                exit(1);
            }
            if(ppuAddr == 0x3f10){ ppuMemory[0x3f00] = byte; }
            else{ ppuMemory[ppuAddr] = byte; }
            if(ppuCtrl.vramAddressIncrement)
                ppuAddr = (ppuAddr + 32) & VRAM_MAX;
            else
                ppuAddr = (ppuAddr + 1) & VRAM_MAX;
        }
    }
    else if(addr == 0x4014){
        //printf("write %02x to $4014 (OAM DMA)\n", byte);
        int ptr = oamAddr;
        for(int i = 0; i < 256; i++){
            oam[ptr] = memory[0x200 + i];
            if(++ptr > 255) ptr = 0;
        }

        dmaFlag = 1;

        // TODO
        // OAM DMA is how sprites in oam are updated
        // writing to 4014 transfers 256 from $XX00-$XXFF of cpu to OAM.
        // the CPU is suspended during the transfer. It takes 513 or 514 cycles.
        // the data is transferred to OAM starting at the current OAM ADDR
    }
    else if(addr == 0x4000){
        setEnvelope(0, byte & 0x3f);
        setDutyCycle(0, byte >> 6);
    }
    else if(addr == 0x4001){
        setSweep(0, byte);
    }
    else if(addr == 0x4002){
        setTimerLow(0, byte);
    }
    else if(addr == 0x4003){
        setTimerHigh(0, byte & 7);
        setLengthCounter(0, byte >> 3);
    }
    else if(addr == 0x4004){
        setEnvelope(1, byte & 0x3f);
        setDutyCycle(1, byte >> 6);
    }
    else if(addr == 0x4005){
        setSweep(1, byte);
    }
    else if(addr == 0x4006){
        setTimerLow(1, byte);
    }
    else if(addr == 0x4007){
        setTimerHigh(1, byte & 7);
        setLengthCounter(1, byte >> 3);
    }
    else if(addr == 0x4015){
        setEnable(0, byte & 1);
        setEnable(1, (byte >> 1) & 1);
    }
    else if(addr == 0x4016){
        //printf("write %02x to $4016 (joystick strobe)\n", byte);
        //printf("shift register loaded with %02x\n", gamepadShiftRegister);
        gamepadShiftRegister = packGamepad();
    }
    else if(addr == 0x4017){
        setFrameCounterPeriod(byte >> 7);
        //printf("write %02x to $4017 (apu frame counter)\n", byte);
    }
    else if(addr >= 0x4018 && addr <= 0x401f){
        //printf("write %02x to $%04x, i/o functionality that is disabled\n", byte, addr);
    }
    else if(addr >= 0x4020){
        printf("attempting to write to unmapped memory ($%04x <= $%02x)\n", addr, byte);
        exit(1);
    }
    else if(addr >= 0x6000){
        printf("attempting to write to cart RAM (not there) ($%04x <= $%02x)\n", addr, byte);
        exit(1);
    }
    else if(addr >= 0x8000){
        printf("attempting to write to PRG ROM ($%04x <= $%02x)\n", addr, byte);
        exit(1);
    }
    else if(addr < 0 || addr > 0xffff){
        printf("attempting to write out of bounds ($%04x <= $%02x)\n", addr, byte);
        exit(1);
    }
    else {
        memory[addr] = byte;
        logWrite(addr);
    }
}

unsigned char adc(unsigned char a, unsigned char b, int carry, struct ProcessorStatus *p){
    unsigned u = a + b + carry;
    unsigned char c = u;
    p->carry = u > 255;
    p->negative = !!(u & 0x80);
    p->zero = c == 0;
    p->overflow = !!(~(a ^ b) & (a ^ c) & 0x80);

    return c;
}

unsigned char sbc(unsigned char a, unsigned char b, int carry, struct ProcessorStatus *p){
    return adc(a, ~b, carry, p);
}

int nextCPUDelay(){
    // this could easily be a table, opcode -> cycles
    int arg1, arg2;
    struct Instruction * ins = fetchInstruction(regs.PC, &arg1, &arg2);
    return ins->cycles;
}

// 7 cycle interrupt sequence, transfer control to NMI vector
void nmiCPU(){
    // ignore the incoming instruction (2 cycles)
    // save 2 byte PC to stack (2 cycles)
    // push status register on stack (1 cycle)
    // set I flag and fetch FFFE (1 cycle)
    // fetch FFFF and update PC (1 cycle)
    memory[0x0100 + regs.S] = regs.PC >> 8;
    regs.S--;
    memory[0x0100 + regs.S] = regs.PC & 0xff;
    regs.S--;
    memory[0x0100 + regs.S] = packProcessorStatus(regs.P);
    regs.S--;
    regs.P.interruptDisable = 1;
    regs.PC = vectors.nmi;
    //printf("NMI executing\n");
}

// fetch next instruction and execute effects
// leaves the CPU in some state after N cycles (see nextCPUDelay)
void stepCPU(){
    int arg1, arg2;
    struct Instruction * ins = fetchInstruction(regs.PC, &arg1, &arg2);
    int size = ins->size;
    int arg21;
    int addr;
    unsigned char m;
    unsigned char c;
    unsigned char bit;
    int lower;
    int upper;

    //printf(" pc=%04x %s ", regs.PC, ins->mnemonic);

    //if(regs.PC == 0xb1ba || regs.PC == 0xb1c6){
    // this comparison leads to resetting the game if mario is too far down
    //if(regs.PC == 0xb1aa){ printf("comparing %02x and %02x\n", regs.A, memory[7]); }
    //if(regs.PC == 0xb1b8){ printf("store 6 in GameEngineSubroutine\n"); }
    //if(regs.PC == 0xdc64){ printf("called PlayerBGCollision. GES = %02x\n", memory[0x0e]); }

    /*
    if(regs.PC == 0x82c5){
        printf("82c5 A=%02x lives=%02x\n", regs.A, memory[0x075a]);
        //cpuFreeze = 1;
        //printf("OperExecutionTree A=%02x OperMode=%02x Task=%02x\n", regs.A, memory[0x770], memory[0x772]);
        //printf("%d AutoControlPlayer\n", cycle);
        //printf("%d BOOM %04x\n", cycle, regs.PC);
        //printf("bcs ResetTitle (carry = %d)\n", regs.P.carry);
    }
    */

    remember(regs.PC);
    regs.PC += size;

    switch(ins->opcode){
        case 0x78: // SEI
            regs.P.interruptDisable = 1;
            break;

        case 0x38: // SEC
            regs.P.carry = 1;
            break;

        case 0xd8: // CLD
            regs.P.decimal = 0;
            break;

        case 0x18: // CLC
            regs.P.carry = 0;
            break;

        case 0xc9: // CMP #$43
            regs.P.carry     = regs.A >= arg1;
            regs.P.zero      = regs.A == arg1;
            c = regs.A - arg1;
            regs.P.negative  = c >> 7;
            break;

        case 0xc5: // CMP $03
            m = memory[arg1];
            regs.P.carry     = regs.A >= m;
            regs.P.zero      = regs.A == m;
            c = regs.A - m;
            regs.P.negative  = c >> 7;
            break;

        case 0xd5: // CMP $03, X
            addr = (arg1 + regs.X) & 0xff;
            m = memory[addr];
            regs.P.carry     = regs.A >= m;
            regs.P.zero      = regs.A == m;
            c = regs.A - m;
            regs.P.negative  = c >> 7;
            break;

        case 0xcd: // CMP $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.P.carry     = regs.A >= m;
            regs.P.zero      = regs.A == m;
            c = regs.A - m;
            regs.P.negative  = c >> 7;
            break;

        case 0xdd: // CMP $0201, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            regs.P.carry     = regs.A >= m;
            regs.P.zero      = regs.A == m;
            c = regs.A - m;
            regs.P.negative  = c >> 7;
            break;

        case 0xd9: // CMP $0201, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            m = readMemory(addr);
            regs.P.carry     = regs.A >= m;
            regs.P.zero      = regs.A == m;
            c = regs.A - m;
            regs.P.negative  = c >> 7;
            break;


        case 0xe0: // CPX #$07
            regs.P.carry     = regs.X >= arg1;
            regs.P.zero      = regs.X == arg1;
            c = regs.X - arg1;
            regs.P.negative  = c >> 7;
            break;

        case 0xe4: // CPX $06
            m = memory[arg1];
            regs.P.carry     = regs.X >= m;
            regs.P.zero      = regs.X == m;
            c = regs.X - m;
            regs.P.negative  = c >> 7;
            break;
            

        case 0xc0: // CPY #$07
            regs.P.carry     = regs.Y >= arg1;
            regs.P.zero      = regs.Y == arg1;
            c = regs.Y - arg1;
            regs.P.negative  = c >> 7;
            break;

        case 0xc4: // CPY $07
            m = memory[arg1];
            regs.P.carry     = regs.Y >= m;
            regs.P.zero      = regs.Y == m;
            c = regs.Y - m;
            regs.P.negative  = c >> 7;
            break;

        case 0xcc: // CPY $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.P.carry     = regs.Y >= m;
            regs.P.zero      = regs.Y == m;
            c = regs.Y - m;
            regs.P.negative  = c >> 7;
            break;
            
        case 0xa9: // LDA #$7f
            regs.A = arg1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xa5: // LDA $25
            regs.A = memory[arg1];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xad: // LDA $0205
            addr = (arg2 << 8) | arg1;
            regs.A = readMemory(addr);
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xb5: // LDA $23, X
            addr = (arg1 + regs.X) & 0xff;
            regs.A = memory[addr];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xbd: // LDA $0205, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            regs.A = readMemory(addr);
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xb1: // LDA ($00), Y
            lower = memory[arg1];
            upper = memory[(arg1+1) & 0xff];
            addr = (upper << 8) | lower;
            addr += regs.Y;
            addr &= 0xffff;
            regs.A = readMemory(addr);
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xb9: // LDA $0233, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            regs.A = readMemory(addr);
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x85: // STA $06
            memory[arg1] = regs.A;
            logWrite(arg1);
            break;

        case 0x95: // STA $06, X
            addr = (arg1 + regs.X) & 0xff;
            memory[addr] = regs.A;
            logWrite(addr);
            break;

        case 0x8d: // STA $0205
            arg21 = (arg2 << 8) | arg1;
            writeMemory(arg21, regs.A);
            break;

        case 0x91: // STA ($06), Y
            lower = memory[arg1];
            upper = memory[(arg1 + 1) & 0xff];
            addr = (upper << 8) | lower;
            addr += regs.Y;
            addr &= 0xffff;
            writeMemory(addr, regs.A);
            break;

        case 0x99: // STA $0205, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            writeMemory(addr, regs.A);
            break;

        case 0x9d: // STA $0205, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            writeMemory(addr, regs.A);
            break;

        case 0xa2: // LDX #$7f
            regs.X = arg1;
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xa6: // LDX $07
            regs.X = memory[arg1];
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xb6: // LDX $07, Y
            addr = (arg1 + regs.Y) & 0xff;
            regs.X = memory[addr];
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xae: // LDX $0203
            addr = (arg2 << 8) | arg1;
            regs.X = readMemory(addr);
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xbe: // LDX $0203, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            regs.X = readMemory(addr);
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xa0: // LDY #$7f
            regs.Y = arg1;
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0xa4: // LDY $07
            regs.Y = memory[arg1];
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0xb4: // LDY $07, X
            addr = (arg1 + regs.X) & 0xff;
            regs.Y = memory[addr];
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0xac: // LDY $0203
            addr = (arg2 << 8) | arg1;
            regs.Y = readMemory(addr);
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0xbc: // LDY $0203, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            regs.Y = readMemory(addr);
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0x86: // STX $07
            memory[arg1] = regs.X;
            logWrite(arg1);
            break;

        case 0x8e: // STX $0201
            addr = (arg2 << 8) | arg1;
            writeMemory(addr, regs.X);
            break;

        case 0x84: // STY $07
            memory[arg1] = regs.Y;
            logWrite(arg1);
            break;

        case 0x94: // STY $07, X
            addr = (arg1 + regs.X) & 0xff;
            memory[addr] = regs.Y;
            logWrite(addr);
            break;

        case 0x8c: // STY $0201
            addr = (arg2 << 8) | arg1;
            writeMemory(addr, regs.Y);
            break;

        case 0x9a: // TXS
            regs.S = regs.X;
            break;

        case 0x8a: // TXA
            regs.A = regs.X;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x98: // TYA
            regs.A = regs.Y;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0xaa: // TAX
            regs.X = regs.A;
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xa8: // TAY
            regs.Y = regs.A;
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0x48: // PHA
            memory[0x0100 + regs.S] = regs.A;
            logWrite(0x0100 + regs.S);
            regs.S--;
            break;

        case 0x68: // PLA
            regs.S++;
            regs.A = memory[0x0100 + regs.S];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x10: // BPL #7 branch if positive (i.e. not negative)
            if(regs.P.negative == 0) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0x30: // BMI #6 branch if minus
            if(regs.P.negative) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0xb0: // BCS #3 branch if carry
            if(regs.P.carry) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0x90: // BCC #3 branch if carry clear
            if(regs.P.carry == 0) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0xd0: // BNE #4 branch if not equal
            if(regs.P.zero == 0) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0xf0: // BEQ #6 branch if equal
            if(regs.P.zero) regs.PC += UNCOMPLEMENT(arg1);
            break;

        case 0x0a: // ASL (shift left, introducing zeros)
            regs.P.carry = regs.A >> 7;
            regs.A = regs.A << 1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x0e: // ASL, $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.P.carry = m >> 7;
            c = m << 1;
            writeMemory(addr, c);
            regs.P.zero =     c == 0;
            regs.P.negative = c >> 7;
            break;

        case 0x4a: // LSR A (shift right, introducing zeros)
            regs.P.carry = regs.A & 1;
            regs.A = regs.A >> 1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x46: // LSR $15
            m = memory[arg1];
            regs.P.carry = m & 1;
            c = m >> 1;
            memory[arg1] = c;
            regs.P.zero     = c == 0;
            regs.P.negative = c >> 7;
            logWrite(arg1);
            break;

        case 0x4e: // LSR $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.P.carry = m & 1;
            c = m >> 1;
            regs.P.zero     = c == 0;
            regs.P.negative = c >> 7;
            writeMemory(addr, c);
            break;

        case 0x2a: // ROL A   rotate left
            if(regs.P.carry > 1){
                printf("botched carry bit\n");
                exit(1);
            }
            bit = regs.P.carry;
            regs.P.carry = regs.A >> 7;
            regs.A = (regs.A << 1) | bit;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x26: // ROL $14
            bit = regs.P.carry;
            m = memory[arg1];
            regs.P.carry = m >> 7;
            c = (m << 1) | bit;
            regs.P.zero     = c == 0;
            regs.P.negative = c >> 7;
            memory[arg1] = c;
            logWrite(arg1);
            break;

        case 0x2e: // ROL $0201
            addr = (arg2 << 8) | arg1;
            bit = regs.P.carry;
            m = readMemory(addr);
            regs.P.carry = m >> 7;
            c = (m << 1) | bit;
            regs.P.zero     = c == 0;
            regs.P.negative = c >> 7;
            writeMemory(addr, c);
            break;

        case 0x6a: // ROR A   rotate right
            bit = regs.P.carry;
            regs.P.carry = regs.A & 1;
            regs.A = (regs.A >> 1) | (bit << 7);
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x7e: // ROR $0201, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            bit = regs.P.carry;
            regs.P.carry = m & 1;
            m = (m >> 1) | (bit << 7);
            regs.P.zero     = m == 0;
            regs.P.negative = m >> 7;
            writeMemory(addr, m);
            break;

        case 0x09: // ORA #$1f
            regs.A = regs.A | arg1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x05: // ORA $1f
            regs.A = regs.A | memory[arg1];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x15: // ORA $1f, X
            addr = (arg1 + regs.X) & 0xff;
            regs.A = regs.A | memory[addr];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x0d: // ORA $0203
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.A = regs.A | m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x1d: // ORA $0203, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            regs.A = regs.A | m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x19: // ORA $0203, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            m = readMemory(addr);
            regs.A = regs.A | m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x29: // AND #$1f
            regs.A = regs.A & arg1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x25: // AND $02
            regs.A = regs.A & memory[arg1];
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x2d: // AND $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.A = regs.A & m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x3d: // AND $0201, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            regs.A = regs.A & m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x39: // AND $0201, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            m = readMemory(addr);
            regs.A = regs.A & m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x49: // EOR #$11
            regs.A = regs.A ^ arg1;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;

        case 0x45: // EOR $11
            m = memory[arg1];
            regs.A = regs.A ^ m;
            regs.P.zero     = regs.A == 0;
            regs.P.negative = regs.A >> 7;
            break;


        // In ADC and SBC handler, status bits are handled by the subroutine
        case 0x69: // ADC #$7
            regs.A = adc(regs.A, arg1, regs.P.carry, &regs.P);
            break;

        case 0x65: // ADC $3c
            m = memory[arg1];
            regs.A = adc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0x75: // ADC $3c, X
            addr = (arg1 + regs.X) & 0xff;
            m = memory[addr];
            regs.A = adc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0x6d: // ADC $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.A = adc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0x7d: // ADC $0201, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            regs.A = adc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0x79: // ADC $0201, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            m = readMemory(addr);
            regs.A = adc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0xe9: // SBC #$5
            regs.A = sbc(regs.A, arg1, regs.P.carry, &regs.P);
            break;

        case 0xe5: // SBC $52
            m = memory[arg1];
            regs.A = sbc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0xf5: // SBC $1f, X
            addr = (arg1 + regs.X) & 0xff;
            m = memory[addr];
            regs.A = sbc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0xed: // SBC $0201
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.A = sbc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0xfd: // SBC $0201, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            regs.A = sbc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0xf9: // SBC $0201, Y
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.Y) & 0xffff;
            m = readMemory(addr);
            regs.A = sbc(regs.A, m, regs.P.carry, &regs.P);
            break;

        case 0x24: // BIT $44
            m = memory[arg1];
            regs.P.overflow = (m >> 6) & 1;
            regs.P.negative = (m >> 7) & 1;
            regs.P.zero = (m & regs.A) == 0;
            break;

        case 0x2c: // BIT $0203
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            regs.P.overflow = (m >> 6) & 1;
            regs.P.negative = (m >> 7) & 1;
            regs.P.zero = (m & regs.A) == 0;
            break;
            
        case 0xca: // DEX
            regs.X--;
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0x88: // DEY
            regs.Y--;
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0xe6: // INC $11
            m = memory[arg1];
            memory[arg1] = m + 1;
            logWrite(arg1);
            regs.P.zero     = (m + 1) == 0;
            c = m + 1;
            regs.P.negative = c >> 7;
            break;

        case 0xf6: // INC $11, X
            addr = (arg1 + regs.X) & 0xff;
            m = memory[addr];
            memory[addr] = m + 1;
            logWrite(addr);
            regs.P.zero     = (m + 1) == 0;
            c = m + 1;
            regs.P.negative = c >> 7;
            break;

        case 0xee: // INC $0203   increment memory
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            writeMemory(addr, m + 1);
            regs.P.zero     = (m + 1) == 0;
            c = m + 1;
            regs.P.negative = c >> 7;
            break;

        case 0xfe: // INC $0203, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            writeMemory(addr, m + 1);
            regs.P.zero     = (m + 1) == 0;
            c = m + 1;
            regs.P.negative = c >> 7;
            break;

        case 0xce: // DEC $0203
            addr = (arg2 << 8) | arg1;
            m = readMemory(addr);
            writeMemory(addr, m - 1);
            regs.P.zero     = (m - 1) == 0;
            c = m - 1;
            regs.P.negative = c >> 7;
            break;

        case 0xc6: // DEC $03
            m = memory[arg1];
            memory[arg1] = m - 1;
            logWrite(arg1);
            regs.P.zero     = (m - 1) == 0;
            c = m - 1;
            regs.P.negative = c >> 7;
            break;

        case 0xd6: // DEC $11, X
            addr = (arg1 + regs.X) & 0xff;
            m = memory[addr];
            memory[addr] = m - 1;
            logWrite(addr);
            regs.P.zero     = (m - 1) == 0;
            c = m - 1;
            regs.P.negative = c >> 7;
            break;

        case 0xde: // DEC $0203, X
            arg21 = (arg2 << 8) | arg1;
            addr = (arg21 + regs.X) & 0xffff;
            m = readMemory(addr);
            writeMemory(addr, m - 1);
            regs.P.zero     = (m - 1) == 0;
            c = m - 1;
            regs.P.negative = c >> 7;
            break;

        case 0xe8: // INX
            regs.X++;
            regs.P.zero     = regs.X == 0;
            regs.P.negative = regs.X >> 7;
            break;

        case 0xc8: // INY
            regs.Y++;
            regs.P.zero     = regs.Y == 0;
            regs.P.negative = regs.Y >> 7;
            break;

        case 0x20: // JSR $8100
            arg21 = (arg2 << 8) | arg1;
//            printf("S=%02x, JSR from %04x to %04x (%s)\n", regs.S, regs.PC, arg21, locationName(arg21));
            if(locationName(arg21)[0] == '?'){
                //printf("unknown location, investigate\n");
                //exit(1);
            }
//if(arg21 == 0x8e19) cpuTimeDilation = 1000;

            addr = regs.PC - 1;
            memory[0x0100 + regs.S] = addr >> 8;
            logWrite(0x0100 + regs.S);
            regs.S--;
            memory[0x0100 + regs.S] = addr & 0xff;
            logWrite(0x0100 + regs.S);
            regs.S--;
            regs.PC = arg21;
            break;

        case 0x60: // RTS
            //printf("S=%02x, RTS from %04x to ", regs.S, regs.PC);
            regs.S++;
            regs.PC = memory[0x0100 + regs.S];
            regs.S++;
            regs.PC |= memory[0x0100 + regs.S] << 8;
            regs.PC++;
            //printf("%04x (%s)\n", regs.PC, locationName(regs.PC));
            break;

        case 0x4c: // JMP $810c
            arg21 = (arg2 << 8) | arg1;
            regs.PC = arg21;
            break;

        case 0x6c: // JMP ($0123)
            arg21 = (arg2 << 8) | arg1;
            lower = readMemory(arg21);
            upper = readMemory((arg21 + 1) & 0xffff);
            addr = (upper << 8) | lower;
            if(addr == 0xb0e6){
//            if(addr == 0x8567){
                printf("(%04x) JMP to %04x (%s)\n", regs.PC, addr, locationName(addr));
                //printf("($04) ($05) = %02x %02x\n", memory[4], memory[5]);
                //printf("ScreenRoutineTask = %02x\n", memory[0x73c]);
            }
            regs.PC = addr;
            break;

        case 0x40: // RTI
            regs.S++;
            m = memory[0x0100 + regs.S];
            regs.S++;
            lower = memory[0x0100 + regs.S];
            regs.S++;
            upper = memory[0x0100 + regs.S];
            addr = (upper << 8) | lower; 
            regs.P = unpackProcessorStatus(m);
            regs.PC = addr;
            //printf("%04x rti\n\n", regs.PC);
            //showCPU();
            break;

        default:
            printf("opcode not implemented (%02x) (%s)\n", ins->opcode, ins->mnemonic);
            exit(1);
    }

    if(timeFreeze) debug();

    //if(timeDilation > ) {debug(); printf("\n");}
}

void readRom(){
    struct NESHeader hdr;

    for(int i = 0; i < 4; i++) hdr.start[i] = rom[i];
    hdr.prg_rom_size = rom[4];
    hdr.chr_rom_size = rom[5];
    hdr.flags6 = rom[6];
    hdr.flags7 = rom[7];
    hdr.flags8 = rom[8];
    hdr.flags9 = rom[9];
    hdr.flags10 = rom[10];
    for(int i = 0 ; i < 5; i++) hdr.unused[i] = rom[11+i];

    printf("{\n");
    printf("\tstart = [%c %c %c %02x]\n", hdr.start[0], hdr.start[1], hdr.start[2], hdr.start[3]);

    printf("\tprg_rom_size = %d\n", hdr.prg_rom_size);
    printf("\tchr_rom_size = %d\n", hdr.chr_rom_size);
    printf("\tflags6 = "); printBits(hdr.flags6); printf("\n");
    printf("\tflags7 = "); printBits(hdr.flags7); printf("\n");
    printf("\tflags8 = "); printBits(hdr.flags8); printf("\n");
    printf("\tflags9 = "); printBits(hdr.flags9); printf("\n");
    printf("\tflags10 = "); printBits(hdr.flags10); printf("\n");
    printf("\tunused = [");
    for(int i = 0; i < 5; i++){
        int c = hdr.unused[i];
        if(isprint(c)) printf("%c", c);
        else printf("%02x", c);
        if(i < 4) printf(" ");
    }
    printf("]\n");
    printf("}\n\n");


    unsigned char mapper = 0;
    mapper = hdr.flags6 >> 4;
    mapper |= hdr.flags7 & 0xf0;
    printf("mapper = %d\n", mapper);

    int prgsize = hdr.prg_rom_size * 16 * 1024;
    int chrsize = hdr.chr_rom_size * 8 * 1024;

    printf("prg rom size = %d\n", prgsize);
    printf("chr rom size = %d\n", chrsize);

    prg = malloc(prgsize);
    chr = malloc(chrsize);

    for(int i = 0; i < prgsize; i++) {
        prg[i] = rom[16 + i];
        memory[0x8000 + i] = rom[16 + i];
    }

    for(int i = 0; i < chrsize; i++) {
        chr[i] = rom[16 + prgsize + i];
        ppuMemory[i] = chr[i];
    }

    vectors.nmi   = (memory[0xfffb] << 8) | memory[0xfffa];
    vectors.reset = (memory[0xfffd] << 8) | memory[0xfffc];
    vectors.irq   = (memory[0xffff] << 8) | memory[0xfffe];

    for(int j = 0; j < prgsize/16; j++){
        printf("%4x: ", j*16);
        for(int i = 0; i < 16; i++){
            printf("%02x ", prg[j*16 + i]);
        }
        printf("\n");
    }

    printf("nmi   @ $%04x\n", vectors.nmi);
    printf("reset @ $%04x\n", vectors.reset);
    printf("irq   @ $%04x\n", vectors.irq);

}


/*
void dumpRom(){
    FILE* file = fopen("rom.nes", "r");
    if(file==NULL){
        printf("failed to open rom\n");
        exit(1);
    }

    int nread;
    unsigned char buf[16];

    int total = 0;

    printf("unsigned char rom[2561 * 16] = {\n");

    for(;;){
        nread = fread(buf, 1, 16, file);
        if(nread < 0){
            printf("read error\n");
            exit(1);
        }

        if(nread == 0) break;

        total += nread;

        printf("\t");
        for(int i=0; i < nread; i++){
            int c = buf[i];
            if(c == '\\') printf("%3d", c);
            else if(c == '\'') printf("%3d", c);
            else if(isprint(c)) printf("'%c'", c);
            else printf("%3d", c);
            if(i != nread - 1) printf(",");
        }

        if(total == 40976){
            printf("\n};\n");
            break;
        }
        else{
            printf(",\n");
        }

    }

}
*/


// if the CPU reads $2002, then poll the ppuStatus bits and clear bit 7
// if the PPU reaches dot 1 of line 241, set bit 7
//    also at this time, generate an NMI if NMI-on-vblank enabled
// if the PPU dot 1 of "pre-render line" clear bit 7

void writeScreen(int row, int col, int r, int g, int b){
    if(row < 0 || row > screenH - 1){
        printf("row out of bounds (%d) screenH = %d\n", row, screenH);
        exit(-1);
    }
    else if(col < 0 || col > screenW - 1){
        printf("col out of bounds (%d)\n", col);
        exit(-1);
    }
    unsigned char * pixel = screenImg.data + row*screenW*4 + col*4;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    //pixel[3] = 255;
}


int encodePatternAddress(int half, int tileNo, int bitplane, int rowNo){
    int addr = 0;
    addr |= half << 12;
    addr |= tileNo << 4;
    addr |= bitplane << 3;
    addr |= rowNo;
    return addr;
}


void resetPPU(){
}

void uploadScreen(){
    UpdateTexture(screenTex, screenImg.data);
}

void DrawVar(int n, const char * name, int addr, int size, Color color){
    char msg[256];

    if(size == 1)
        sprintf(msg, "%s = %02x", name, memory[addr]);
    else if(size == 2)
        sprintf(msg, "%s = %02x %02x", name, memory[addr], memory[addr+1]);
    else
        return;

    DrawText(msg, 2, n * 12, 12, color);
}

/*
void DrawCoinDisplay(int n){
    char msg[16];
    DrawText("Coin = ", 2, n * 12, 12, WHITE);
    for(int i=0; i<28; i++){
        sprintf(msg, "%02x", memory[0x07dd + i]);
        DrawText(msg, 2 + 40 + i * 16, n * 12, 12, WHITE);
    }
}
*/


// algorithm:
// for each line
// set coarse X high bits of scrollX
// fetch first slice (pattern data and palette number)
// load 8 pixels into a queue.
// drop fine_x (low 3 bits of scrollX) from queue.
// for each dot, dequeue 1 pixels worth of bits and output.
// if queue is empty, increment coarse X.
// if coarse X goes over 31, set coarse X to zero and switch nametable
// continue until dot reaches end of line.
// repeat for all lines.

int coarseX = 0;
struct RGB slicePalette[4]; // 0 1 2 or 3
int renderBase = 0;
unsigned char sliceQueue0 = 0; // up to 8 bits, dequeue 2 at a time
unsigned char sliceQueue1 = 0; // up to 8 bits, dequeue 2 at a time
int sliceQueueSize = 0; // number of pairs of bits

int dequeue(){
    if(sliceQueueSize == 0){
        printf("please fix the code, you should not be dequeuing from empty\n");
        exit(1);
    }

    int bit0 = sliceQueue0 >> 7;
    int bit1 = sliceQueue1 >> 7;
    sliceQueue0 <<= 1;
    sliceQueue1 <<= 1;
    sliceQueueSize--;
    return (bit1 << 1) | bit0;
}

void fetchSlice2(int table, int patternNo, int line, unsigned char* plane0, unsigned char* plane1){
    int addr = table ? 0x1000 : 0x0000;
    addr += patternNo * 16;
    addr += line;
    *plane0 = ppuMemory[addr];
    *plane1 = ppuMemory[addr + 8];
}

// form a number 0 to 3 using two bits from a slice
int extractFromSlice(int x, unsigned char plane0, unsigned char plane1){
    int bit0 = (plane0 >> (7 - x)) & 1;
    int bit1 = (plane1 >> (7 - x)) & 1;
    return (bit1 << 1) | bit0;
}


// return a final color index for sprites here, or -1 if transparent
int loopOverSpritesHere(int bg, int line, int dot){
    int table = ppuCtrl.spritePatternAddress; // 0 or 1
    int result = -1;
    unsigned char code;
    for(int i = 0; i < 64; i++){
        int y = oam[i*4];
        int x = oam[i*4 + 3];
        if(y <= line && line <= y + 7 && x <= dot && dot <= x + 7){
            int patternNo = oam[i*4 + 1];
            unsigned char attr = oam[i*4 + 2];
            unsigned char plane0;
            unsigned char plane1;
            if((attr >> 7) & 1){
                fetchSlice2(table, patternNo, 7 - (line - y), &plane0, &plane1);
            }
            else{
                fetchSlice2(table, patternNo, line - y, &plane0, &plane1);
            }
            if((attr >> 6) & 1){
                code = extractFromSlice(7 - (dot - x), plane0, plane1);
            }
            else{
                code = extractFromSlice(dot - x, plane0, plane1);
            }
            if(code != 0x00){
                int behindBg = (attr >> 5) & 1;
                if(!(bg && behindBg)){
                    int palBase = 0x3f10 + 4*(attr & 3);
                    result = ppuMemory[palBase + code];
                }
            }
        }
    }
    return result;
}

void fetchSlice(int line, int coarseX){

    // get palette
    unsigned char attr = ppuMemory[renderBase + 0x03c0 + (line/32)*8 + coarseX/4];
    //int paletteNo = (attr >> 0) & 3; // top left
    //int paletteNo = (attr >> 2) & 3; // top right
    //int paletteNo = (attr >> 4) & 3; // bottom left
    //int paletteNo = (attr >> 6) & 3;  // bototm right
    int paletteNo = 0;
    int row = (line / 16) & 1; // odd row yes no
    int col = (coarseX / 2) & 1; // odd column yes no
    if(!row && !col) paletteNo = (attr >> 0) & 3;
    if(!row &&  col) paletteNo = (attr >> 2) & 3;
    if( row && !col) paletteNo = (attr >> 4) & 3;
    if( row &&  col) paletteNo = (attr >> 6) & 3;
    int palBase = 0x3f00 + paletteNo * 4;
    for(int i = 0; i < 4; i++){
        int code = ppuMemory[palBase + i];
        slicePalette[i] = colors[code];
    }
    slicePalette[0] = colors[ppuMemory[0x3f00]]; // universal bg color

    // get slice
    int patternNo   = ppuMemory[renderBase + (line/8)*32 + coarseX];
    int patternBase = ppuCtrl.bgPatternAddress ? 0x1000 : 0x0000;
    int sliceNo = line % 8;

    sliceQueue0 = ppuMemory[patternBase + patternNo*16 + sliceNo];
    sliceQueue1 = ppuMemory[patternBase + patternNo*16 + 8 + sliceNo];
    sliceQueueSize = 8;

}




int nmiComing = 0;
int nmiHappening = 0;
int cpuDots = 1;
int triplet = 3;
int stepPPU(){ // outputs 1 dot, return 1 if instruction completed

    if(dmaFlag){
        dmaFlag = 0;
        //cpuDots += 3 * 513;
    }

    if(dot == 0 && scanline < 240){
//        findSpritesOnLine(scanline + 1); // 
    }

    // process 1 dot here
    // compute background pixel
    // consult scanline's sprite buffer
    if(dot < 256 && scanline >= 1 && scanline <= 240){

        if(dot == 0){
            renderBase = ppuCtrl.nametableBase ? 0x2400 : 0x2000;
            coarseX = ppuScrollX / 8;
            fetchSlice(scanline - 1, coarseX);
            for(int i = 0; i < ppuFineX; i++) dequeue();
        }

        if(sliceQueueSize == 0){
            if(coarseX == 31){
                coarseX = 0;
                renderBase = (renderBase == 0x2000) ? 0x2400 : 0x2000;
            }
            else{
                coarseX++;
            }
            fetchSlice(scanline - 1, coarseX);
        }

        int bg = dequeue(); // the background pixel
        int fg = loopOverSpritesHere(bg, scanline - 1, dot); // sprite pixel, if any

        struct RGB color;
        if(fg < 0){
            color = slicePalette[bg];
        }
        else{
            color = colors[fg];
        }
        writeScreen(scanline-1, dot, color.r, color.g, color.b);

    }

    dot++;
    if(dot == 341){
        dot = 0;
        scanline++;

        if(scanline == 241){
            ppuStatus.inVblank = 1;
            if(ppuCtrl.nmiOutput){ nmiComing = 1; }
        }

        if(scanline == 262){
            scanline = 0;
            ppuStatus.inVblank = 0;
            ppuStatus.spriteZeroHit = 0;
            frameNo++;
        }
    }

    if(dot == oam[3] && scanline - 1 == oam[0] + 5 && /* sprite0 and bg not transparent */ 1){
        ppuStatus.spriteZeroHit = 1;
    }


    triplet--;
    if(triplet == 0){
        triplet = 3;
        apuFrameHalfClock();
    }


    // make 1 dots of progress on CPU
    cpuDots--;
    if(cpuDots == 0){
        if(nmiHappening){
            nmiCPU();
            cpuDots = 3 * nextCPUDelay();
            nmiHappening = 0;
            // inhibit nmi now so 1 instruction at least gets executed
        }
        else if(nmiComing){
            stepCPU();
            cpuDots = 3 * 7;
            nmiHappening = 1;
            nmiComing = 0;
        }
        else{
            stepCPU();
            cpuDots = 3 * nextCPUDelay();
            // clear nmi inhibiting
        }

        return 1;
    }

    return 0;
}



void drawByte(int x, int y, unsigned char byte){
    char msg[8];
    sprintf(msg, "%02x", byte);
    DrawText(msg, x*14, y*12, 10, WHITE);
}

void drawSwatch(int x, int y, int pal){
    int index = ppuMemory[0x3f00 + pal];
    struct RGB *color = &colors[index];
    struct Color c = {color->r, color->g, color->b, 255};
    DrawRectangle(x, y, 32, 32, c);
}

void drawPalettes(int baseX, int baseY){
    // universal bg color
    drawSwatch(32*0,32*0,0);
    // bg palette zero
    drawSwatch(32*0,32*1,1);
    drawSwatch(32*0,32*2,2);
    drawSwatch(32*0,32*3,3);

    drawSwatch(32*1,32*0,4);
    // bg palette 1
    drawSwatch(32*1,32*1,5);
    drawSwatch(32*1,32*2,6);
    drawSwatch(32*1,32*3,7);

    drawSwatch(32*2,32*0,8);
    // bg palette 2
    drawSwatch(32*2,32*1,9);
    drawSwatch(32*2,32*2,10);
    drawSwatch(32*2,32*3,11);

    drawSwatch(32*3,32*0,12);
    // bg palette 3
    drawSwatch(32*3,32*1,13);
    drawSwatch(32*3,32*2,14);
    drawSwatch(32*3,32*3,15);

    drawSwatch(32*0,32*(5+0),0);
    // sprite palette zero
    drawSwatch(32*0,32*(5+1),17);
    drawSwatch(32*0,32*(5+2),18);
    drawSwatch(32*0,32*(5+3),19);

    drawSwatch(32*1,32*(5+0),4);
    // sprite palette one
    drawSwatch(32*1,32*(5+1),21);
    drawSwatch(32*1,32*(5+2),22);
    drawSwatch(32*1,32*(5+3),23);

    drawSwatch(32*2,32*(5+0),8);
    // sprite palette zero
    drawSwatch(32*2,32*(5+1),25);
    drawSwatch(32*2,32*(5+2),26);
    drawSwatch(32*2,32*(5+3),27);

    drawSwatch(32*3,32*(5+0),12);
    // sprite palette one
    drawSwatch(32*3,32*(5+1),29);
    drawSwatch(32*3,32*(5+2),30);
    drawSwatch(32*3,32*(5+3),31);
}

mtx_t audio_mutex;

#define AUDIO_BUFFER_SIZE 4096
float audio_buffer[AUDIO_BUFFER_SIZE];
int audio_buffer_ptr = 0;
int audio_buffer_base = 0;
int audio_buffer_amount = 0;


float t = 0;

void generate(int numSamples){
    mtx_lock(&audio_mutex);

    if(numSamples >= AUDIO_BUFFER_SIZE - audio_buffer_amount){
        printf("audio buffer overflow :(\n");
        return;
    }

    if(AUDIO_BUFFER_SIZE - audio_buffer_ptr < numSamples){
        int half1 = AUDIO_BUFFER_SIZE - audio_buffer_ptr;
        int half2 = numSamples - half1;
        synth(&audio_buffer[audio_buffer_ptr], half1);
        synth(&audio_buffer[0], half2);
        audio_buffer_ptr = half2;
        audio_buffer_amount += numSamples;
    }
    else{
        synth(&audio_buffer[audio_buffer_ptr], numSamples);
        audio_buffer_ptr += numSamples;
        audio_buffer_amount += numSamples;
        if(audio_buffer_ptr == AUDIO_BUFFER_SIZE) audio_buffer_ptr = 0;
    }

    mtx_unlock(&audio_mutex);

}

void AudioCb(void *buffer, unsigned int numWanted){
    int16_t *out = buffer;
    float amplitude;

    //printf("audio cb, wanted = %u\n", numWanted);

    if(audio_buffer_amount < numWanted){
        printf("audio drop out :(\n");
        for(int i = 0; i < numWanted; i++){
            out[i] = 0;
        }
    }
    else{
        mtx_lock(&audio_mutex);
        for(int i = 0; i < numWanted; i++){
            amplitude = audio_buffer[audio_buffer_base];
            amplitude = amplitude > 1.0f ? 1.0 : amplitude;
            amplitude = amplitude < -1.0f ? -1.0 : amplitude;
            out[i] = amplitude * INT16_MAX;
            audio_buffer_base++;
            audio_buffer_amount--;
            if(audio_buffer_base == AUDIO_BUFFER_SIZE) audio_buffer_base = 0;
        }
        mtx_unlock(&audio_mutex);
    }
}


int main(){

    InitAudioDevice();
    if(IsAudioDeviceReady() == 0){
        printf("raylib: audio not ready\n");
        exit(1);
    }

    //SetAudioStreamBufferSizeDefault(AUDIO_BUFFER_SIZE);
    mtx_init(&audio_mutex, mtx_plain);
    AudioStream stream = LoadAudioStream(44100, 16, 1);
    SetAudioStreamCallback(stream, AudioCb);
    PlayAudioStream(stream);

    readRom();
    resetCPU();
    showCPU();

    InitWindow(screenW * screenScale, screenH * screenScale, "Zintendo Entertainment System");
    SetTargetFPS(60);

    screenImg = GenImageColor(screenW,screenH,BLUE);
    screenTex = LoadTextureFromImage(screenImg);

    // screenImg.format probably = R8G8B8A8
    printf("screenImg.width   = %d\n", screenImg.width);
    printf("screenImg.height  = %d\n", screenImg.height);
    printf("screenImg.mipmaps = %d\n", screenImg.mipmaps);
    printf("screenImg.format  = %d\n", screenImg.format);


    int stepFlag = 0;
    int skipToNMI = 0;
    int skipToRTS = 0;
    int showNametables = 0;
    int showVisual = 1;
    int showPalettes = 0;
    int showMemory = 0;

    int key = 0;

    while(!WindowShouldClose()) {

        while(audio_buffer_amount < 2000){
            //printf("generating 256");
            generate(256);
        }

        //printf("Player X pos = %02x\n", memory[0x86]);
        //printf("Player X vel = %u\n", memory[0x57]);

        pollGamepad();

        if(stepFlag){
            while(stepPPU()==0);
            stepFlag = 0;
        }
        else if(skipToNMI){
            while(nmiHappening == 0) stepPPU();
            skipToNMI = 0;
            timeFreeze = 1;
            timeDilation = 200000;
        }
        else{
            // 1 frame is 262 lines, each line is 341 dots.
            // if the next CPU instruction would take N cycles
            // the effects must be done in N*3 dots. So after each CPU cycle
            // reset a dot counter.
            int normalSteps = 1 * 262 * 341;
            int dotsPerFrame = normalSteps / timeDilation;
            if(dotsPerFrame < 1) dotsPerFrame = 1;
            if(!skipToRTS && timeFreeze) dotsPerFrame = 0;
            for(int i = 0; i < dotsPerFrame; i++){
                stepPPU();
                //if(regs.PC == 0x8014){ timeFreeze = 1; break; }
                //if(regs.PC == 0x813b && frameNo > 800){ timeFreeze = 1; break; }
                //if(regs.PC == 0x86ff){ timeFreeze = 1; break; }
                //if(regs.PC == 0xefbe){ timeFreeze = 1; break; }
                //if(regs.PC == 0x8e92 && frameNo >= 28){ timeFreeze = 1; break; }
                //if(regs.PC == 0x93fc){ timeFreeze = 1; break; }
                //if(regs.PC == 0x8082){ timeFreeze = 1; break; }
                //if(regs.PC == 0x8227){ timeFreeze = 1; break; }
                if(skipToRTS && memory[regs.PC] == 0x60){
                    skipToRTS = 0;
                    timeFreeze = 1;
                    timeDilation = 200000;
                    break;
                }
            }
        }

        if(IsKeyPressed(KEY_FIVE)){ timeDilation = 1; }
        if(IsKeyPressed(KEY_FOUR)){ timeDilation = 10; }
        if(IsKeyPressed(KEY_THREE)){ timeDilation = 1000; }
        if(IsKeyPressed(KEY_TWO)){ timeDilation = 5000; }
        if(IsKeyPressed(KEY_ONE)){ timeDilation = 200000; }
        if(IsKeyPressed(KEY_F1)){ showMemory = !showMemory; }
        if(IsKeyPressed(KEY_F2)){ showVisual = !showVisual; }
        if(IsKeyPressed(KEY_F3)){ showPalettes = !showPalettes; }
        if(IsKeyPressed(KEY_F4)){ showNametables = !showNametables; }
        //if(IsKeyPressed(KEY_TAB)){ silence = !silence; }
        if(IsKeyPressed(KEY_N)){ skipToNMI = 1; }
        if(IsKeyPressed(KEY_R)){ skipToRTS = 1; timeDilation = 1; }
        if(IsKeyPressed(KEY_F)){ timeFreeze = !timeFreeze; }
        if(timeFreeze && IsKeyPressed(KEY_ENTER)){ stepFlag = 1; }

/*
        if(IsKeyDown(KEY_Z)){ setEnable(1); setFrequency(220.0); key=1; }
        if(key==1 && IsKeyUp(KEY_Z)){ setEnable(0); key=0; }
        if(IsKeyDown(KEY_X)){ setEnable(1); setFrequency(246.9); key=2; }
        if(key==2 && IsKeyUp(KEY_X)){ setEnable(0); key=0; }
        if(IsKeyDown(KEY_C)){ setEnable(1); setFrequency(277.2); key=3; }
        if(key==3 && IsKeyUp(KEY_C)){ setEnable(0); key=0; }
        if(IsKeyDown(KEY_V)){ setEnable(1); setFrequency(293.7); key=4; }
        if(key==4 && IsKeyUp(KEY_V)){ setEnable(0); key=0; }
        if(IsKeyDown(KEY_B)){ setEnable(1); setFrequency(329.6); key=5; }
        if(key==5 && IsKeyUp(KEY_B)){ setEnable(0); key=0; }
*/

        uploadScreen();

        BeginDrawing();
            ClearBackground(BLUE);


        if(showMemory){

        int div = 0;
        for(int i = writeLogPtr-1; i >= 0; i--){
            int addr = writeLog[i];
            int x = (addr % 64) + 1;
            int y = addr / 64;
            Color c = {0, 0, 0, 255};
            c.r = 128 + (127 * (WRITELOG_SIZE - div) / WRITELOG_SIZE);
            div++;
            if(addr < 0xc00){
                DrawRectangle(14*x-1, 12*y-1, 14-1, 12-1, c);
                if(i==writeLogPtr-1){
                    DrawRing((Vector2){14*x+6,12*y+5}, 20, 24, 0, 360, 24, GREEN);
                }
            }
            else{
                printf("can't write some of the log\n");
            }
        }


        //07a2, y = 07a2 / 256, x = 07a2 % 256
        //int track = 0x0045;//player moving direction, check
        //int track = 0x1d; // player state
        //int track = 0x86; // player X position
        //int track = 0x06a1; // metatilebuffer (11 cells)
        int track = 0x0776; // player offscreen bits
        DrawRing((Vector2){14*(track%64 + 1)+6,12*(track/64)+5}, 20, 24, 0, 360, 24, PURPLE);

        for(int j = 0; j < 48; j++){
            if(j%4 == 0) drawByte(0, j, j/4);
            for(int i = 0; i < 64; i++){
                int level = memory[j*64 + i];
                drawByte(i+1, j, level);
            }
        }


        //drawByte(320*3/16 - 1, 240*3/16 - 1, 0);
        for(int i = 0xff; i > regs.S; i--){
            drawByte(320*3/14 - 1, 240*3/12 - 1 - (0xff - i), memory[0x0100 + i]);
        }

        DrawText("1 = turtle slow", 2, 240*3 - 12*7, 10, WHITE);
        DrawText("2 = turtle slow+", 2, 240*3 - 12*6, 10, WHITE);
        DrawText("3 = fast", 2, 240*3 - 12*5, 10, WHITE);
        DrawText("4 = blazing", 2, 240*3 - 12*4, 10, WHITE);
        DrawText("5 = ludicrous", 2, 240*3 - 12*3, 10, WHITE);

        DrawText("F1: hide/show cpu memory", 100, 240*3 - 12*10, 10, WHITE);
        DrawText("F2: hide/show visual", 100, 240*3 - 12*9, 10, WHITE);
        DrawText("F3: hide/show palettes", 100, 240*3 - 12*8, 10, WHITE);
        DrawText("F4: hide/show nametable scan", 100, 240*3 - 12*7, 10, WHITE);
        DrawText("F: freeze/unfreeze", 100, 240*3 - 12*6, 10, WHITE);
        DrawText("Enter: exec 1 instruction", 100, 240*3 - 12*5, 10, WHITE);
        DrawText("R: skip to RTS and freeze", 100, 240*3 - 12*4, 10, WHITE);
        DrawText("N: skip to NMI and freeze", 100, 240*3 - 12*3, 10, WHITE);

        DrawText(TextFormat("frameNo = %d",frameNo), 2, 240*3 - 16, 10, WHITE);

        }


        if(showNametables){
        int per = 61;
        for(int i = 0; i < 0x400; i++){
            unsigned char l = per * ppuMemory[0x2000 + i];
            Color c = {l,l,l,255};
            DrawRectangle(100 + 12*(i%32), 200 + 12*(i/32), 12, 12, c);
        }

        for(int i = 0; i < 0x400; i++){
            unsigned char l = per * ppuMemory[0x2400 + i];
            Color c = {l,l,l,255};
            DrawRectangle(500 + 12*(i%32), 200 + 12*(i/32), 12, 12, c);
        }

        DrawRectangleLines(100 + 3*ppuScrollX/2, 200, 32*12, 32*12, GREEN);

        for(int s = 0; s <= 15; s++){
            int x = oam[s*4 + 3];
            int y = oam[s*4 + 0];
            DrawRing((Vector2){100 + 8 + 3*x/2, 200 + 8 + 3*y/2}, 6, 8, 0, 360, 24, GOLD);
        }

        }

        if(showVisual){
            DrawTextureEx(screenTex, (Vector2){96,0}, 0.0f, 3, WHITE);

            if(showMemory){
                DrawRing((Vector2){96 + 3*dot + 4, 4 + 3*(scanline - 1)}, 6, 8, 0, 360, 24, BLUE);
                for(int s = 0; s < 64; s++){
                    int x = oam[s*4 + 3];
                    int y = oam[s*4 + 0];
                    DrawRectangleLines(96 + 3*x, 3*y, 3*8, 3*8, RED);
                }
            }
        }

        if(showPalettes)
            drawPalettes(0,0);




        EndDrawing();

    }

    mtx_destroy(&audio_mutex);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow(); 

    return 0;
}


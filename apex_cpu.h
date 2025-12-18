#ifndef APEX_CPU_H
#define APEX_CPU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FALSE 0
#define TRUE 1

#define ARCH_REG_FILE_SIZE 32
#define PHYS_REG_FILE_SIZE 42
#define CC_REG_FILE_SIZE 28

#define ROB_SIZE 16
#define INT_RS_SIZE 8
#define MUL_RS_SIZE 4
#define LSQ_SIZE 6
#define BIS_SIZE 8

#define DATA_MEMORY_SIZE 4096
#define CODE_MEMORY_SIZE 1024 

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR, OP_XOR,
    OP_ADDL, OP_SUBL, OP_CML, OP_CMP,
    OP_LOAD, OP_STORE, OP_MOVC,
    OP_JUMP, OP_JAL, OP_RET, OP_JALP,
    OP_BZ, OP_BNZ, OP_BP, OP_BN,
    OP_NOP, OP_HALT, OP_INVALID
} Opcode;


typedef struct Instruction {
    Opcode opcode;
    char opcodeStr[16];
    int pc;
    
    int rd, rs1, rs2, imm;
    
    int physRd, physRs1, physRs2;
    int physCc, physSrcCc;
    
    int rs1Ready, rs2Ready;
    int rs1Value, rs2Value;
    
    int flagsValue, flagsReady;
    
    int robIndex, lsqIndex, bisIndex;
    
    int memoryAddress;
    int predictedTaken;
    int predictedTarget;
    char predictionInfo[64];
} Instruction;

typedef struct {
    int value;
    int valid;
    int allocated;
} PhysicalRegister;

typedef struct {
    Instruction* instr;
    int status; 
    int archRd;
    int physRd;
    int oldPhysRd;
    int writesCc;
    int physCc;
    int oldPhysCc;
    int isBranch;
    int bisIndex;
    int lsqIndex;
} RobEntry;

typedef struct {
    int busy;
    Instruction* instr;
    int dispatchTime;
} RsEntry;

typedef struct {
    int allocated;
    Instruction* instr;
    int memAddress;
    int addressValid;
    int storeData;
    int dataValid;
} LsqEntry;

typedef struct {
    int items[PHYS_REG_FILE_SIZE + 1]; 
    int head;
    int tail;
    int count;
} IntQueue;

typedef struct {
    int items[16];
    int top;
} IntStack;

typedef struct {
    int branchPc;
    int robTailSnapshot;
    int ratSnapshot[ARCH_REG_FILE_SIZE];
    int ratCcSnapshot;
    IntQueue freeListSnapshot;
    IntQueue freeListCcSnapshot;
} BisEntry;

typedef struct {
    int tagPc;
    int targetAddress;
    int history;
    int valid;
    int lruTime;
} BtbEntry;

typedef struct {
    int valid;
    int tagPc;
    int targetAddress;
    int lruTime;
} CtpEntry;

typedef struct {
    int physRegTag;
    int value;
    int isCc;
} ForwardingData;

typedef struct {
    int pc;
    int clock;
    int simulationHalted;
    int instructionsRetired;

    // *** NEW FLAG ***
    int predictor_enabled;
    
    int arf[ARCH_REG_FILE_SIZE];
    int rat[ARCH_REG_FILE_SIZE];
    int ratCc;
    
    PhysicalRegister prf[PHYS_REG_FILE_SIZE];
    PhysicalRegister cprf[CC_REG_FILE_SIZE];
    
    IntQueue freeListPrf;
    IntQueue freeListCprf;
    
    RobEntry rob[ROB_SIZE];
    int robHead, robTail, robCount;
    
    RsEntry intRs[INT_RS_SIZE];
    RsEntry mulRs[MUL_RS_SIZE];
    
    LsqEntry lsq[LSQ_SIZE];
    int lsqHead, lsqTail, lsqCount;
    
    BisEntry bis[BIS_SIZE];
    int bisHead, bisTail, bisCount;
    
    BtbEntry btb[8];
    CtpEntry ctp[4];
    IntStack rap;
    
    Instruction *fetch1Latch;
    Instruction *fetch2Latch;
    Instruction *dispatchLatch;
    Instruction *intFuLatch;
    Instruction *mulFuLatch;
    
    Instruction *mulPipeline[3];
    Instruction *mauPipeline[2];
    
    int dataMemory[DATA_MEMORY_SIZE];
    Instruction codeMemory[CODE_MEMORY_SIZE];
    
    ForwardingData forwardingBuffer[16];
    int forwardingCount;
    
    int fetchStalled;
    int globalDispatchCounter;
    int wasFlushed;
    int wasStalled;
} ApexCpu;

void cpu_init(ApexCpu* cpu);
void cpu_load_program(ApexCpu* cpu, const char* filename);
void cpu_simulate_cycle(ApexCpu* cpu);
void cpu_display(ApexCpu* cpu);
void cpu_display_all_stages(ApexCpu* cpu);
void cpu_set_memory(ApexCpu* cpu, int address, int value);

#endif
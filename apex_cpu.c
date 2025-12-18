/*
 * apex_cpu.c
 * Contains APEX CPU pipeline implementation
 */
#include "apex_cpu.h"

// --------------------------------------------------------------------
// CONFIGURATION
// --------------------------------------------------------------------
#define MAX_CYCLES 200 

Instruction* create_instruction() {
    Instruction* instr = (Instruction*)malloc(sizeof(Instruction));
    memset(instr, 0, sizeof(Instruction));
    instr->opcode = OP_NOP;
    strcpy(instr->opcodeStr, "NOP");
    instr->rd = -1; instr->rs1 = -1; instr->rs2 = -1;
    instr->physRd = -1; instr->physRs1 = -1; instr->physRs2 = -1;
    instr->physCc = -1; instr->physSrcCc = -1;
    instr->robIndex = -1; instr->lsqIndex = -1; instr->bisIndex = -1;
    return instr;
}

void print_instruction_str(Instruction* instr, char* buffer) {
    if (!instr) {
        strcpy(buffer, "(Empty)");
        return;
    }
    sprintf(buffer, "%s", instr->opcodeStr);
    if (instr->rd != -1) sprintf(buffer + strlen(buffer), " R%d", instr->rd);
    if (instr->rs1 != -1) sprintf(buffer + strlen(buffer), " R%d", instr->rs1);
    if (instr->rs2 != -1) sprintf(buffer + strlen(buffer), " R%d", instr->rs2);
    if (instr->imm != 0) sprintf(buffer + strlen(buffer), " #%d", instr->imm);
}

void queue_init(IntQueue* q) {
    q->head = 0; 
    q->tail = 0; 
    q->count = 0;
}
void queue_enqueue(IntQueue* q, int val) {
    q->items[q->tail] = val;
    q->tail = (q->tail + 1) %(PHYS_REG_FILE_SIZE+ 1);
    q->count++;
}
int queue_dequeue(IntQueue* q) {
    if (q->count == 0) return -1;
    int val = q->items[q->head];
    q->head = (q->head + 1) %(PHYS_REG_FILE_SIZE +1);
    q->count--;
    return val;
}
int queue_is_empty(IntQueue* q) { return q->count == 0; }

void stack_init(IntStack* s) { s->top = -1; }
void stack_push(IntStack* s, int val) { s->items[++(s->top)] = val; }
int stack_pop(IntStack* s) { return (s->top >= 0) ? s->items[(s->top)--] : 0; }
int stack_peek(IntStack* s) { return (s->top >= 0) ? s->items[s->top] : 0; }
int stack_is_empty(IntStack* s) { return s->top == -1; }

void cpu_init(ApexCpu* cpu) {
    memset(cpu, 0, sizeof(ApexCpu));
    cpu->pc = 4000;
    
    // Default flag to false (will be set by main)
    cpu->predictor_enabled = 0; 
    
    for(int i=0; i<ARCH_REG_FILE_SIZE; i++) cpu->rat[i] = -1;
    cpu->ratCc = -1;
    
    queue_init(&cpu->freeListPrf);
    for(int i=0; i<PHYS_REG_FILE_SIZE; i++) {
        cpu->prf[i].valid = 1;
        queue_enqueue(&cpu->freeListPrf, i);
    }
    
    queue_init(&cpu->freeListCprf);
    for(int i=0; i<CC_REG_FILE_SIZE; i++) {
        cpu->cprf[i].valid = 1;
        queue_enqueue(&cpu->freeListCprf, i);
    }
    
    for(int i=0; i<ROB_SIZE; i++) {
        cpu->rob[i].instr = NULL;
        cpu->rob[i].archRd = -1; cpu->rob[i].physRd = -1; cpu->rob[i].oldPhysRd = -1;
        cpu->rob[i].physCc = -1; cpu->rob[i].oldPhysCc = -1;
    }
    for(int i=0; i<INT_RS_SIZE; i++) cpu->intRs[i].busy = FALSE;
    for(int i=0; i<MUL_RS_SIZE; i++) cpu->mulRs[i].busy = FALSE;
    for(int i=0; i<LSQ_SIZE; i++) cpu->lsq[i].allocated = FALSE;

    for(int i=0; i<4; i++) {
        cpu->ctp[i].valid = 0;
        cpu->ctp[i].tagPc = -1;
        cpu->ctp[i].targetAddress = 0;
        cpu->ctp[i].lruTime = 0;
    }
    stack_init(&cpu->rap);
    printf("APEX CPU Initialized\n");
}

int ctp_lookup(ApexCpu* cpu, int jalPc) {
    for(int i=0; i<4; i++) {
        if(cpu->ctp[i].valid && cpu->ctp[i].tagPc == jalPc) {
            cpu->ctp[i].lruTime = cpu->clock;
            return cpu->ctp[i].targetAddress;
        }
    }
    return -1;
}

void update_ctp(ApexCpu* cpu, int jalPc, int actualTarget) {
    int match = -1, lru = -1, empty = -1, minTime = 2147483647;
    for(int i=0; i<4; i++) {
        if(!cpu->ctp[i].valid) {
            if(empty == -1) empty = i;
        } else {
            if(cpu->ctp[i].tagPc == jalPc) { match = i; break; }
            if(cpu->ctp[i].lruTime < minTime) { minTime = cpu->ctp[i].lruTime; lru = i; }
        }
    }
    int idx = (match != -1) ? match : ((empty != -1) ? empty : lru);
    cpu->ctp[idx].valid = 1;
    cpu->ctp[idx].tagPc = jalPc;
    cpu->ctp[idx].targetAddress = actualTarget;
    cpu->ctp[idx].lruTime = cpu->clock;
}

int parse_reg(char* str) {
    if(!str) return -1;
    char temp[10]; int j=0;
    for(int i=0; str[i]; i++) if(str[i] >= '0' && str[i] <= '9') temp[j++] = str[i];
    temp[j] = '\0';
    return (j > 0) ? atoi(temp) : -1;
}

int parse_imm(char* str) {
    if(!str) return 0;
    char temp[10]; int j=0;
    for(int i=0; str[i]; i++) if(str[i] == '-' || (str[i] >= '0' && str[i] <= '9')) temp[j++] = str[i];
    temp[j] = '\0';
    return atoi(temp);
}

Opcode get_opcode_enum(char* str) {
    if(!strcmp(str, "ADD")) return OP_ADD;
    if(!strcmp(str, "SUB")) return OP_SUB;
    if(!strcmp(str, "MUL")) return OP_MUL;
    if(!strcmp(str, "AND")) return OP_AND;
    if(!strcmp(str, "OR")) return OP_OR;
    if(!strcmp(str, "XOR")) return OP_XOR;
    if(!strcmp(str, "ADDL")) return OP_ADDL;
    if(!strcmp(str, "SUBL")) return OP_SUBL;
    if(!strcmp(str, "CML")) return OP_CML;
    if(!strcmp(str, "CMP")) return OP_CMP;
    if(!strcmp(str, "LOAD")) return OP_LOAD;
    if(!strcmp(str, "STORE")) return OP_STORE;
    if(!strcmp(str, "MOVC")) return OP_MOVC;
    if(!strcmp(str, "JUMP")) return OP_JUMP;
    if(!strcmp(str, "JAL")) return OP_JAL;
    if(!strcmp(str, "RET")) return OP_RET;
    if(!strcmp(str, "JALP")) return OP_JALP;
    if(!strcmp(str, "BZ")) return OP_BZ;
    if(!strcmp(str, "BNZ")) return OP_BNZ;
    if(!strcmp(str, "BP")) return OP_BP;
    if(!strcmp(str, "BN")) return OP_BN;
    if(!strcmp(str, "NOP")) return OP_NOP;
    if(!strcmp(str, "HALT")) return OP_HALT;
    return OP_INVALID;
}

void cpu_load_program(ApexCpu* cpu, const char* filename) {
    FILE* fp = fopen(filename, "r");
    if(!fp) { printf("Error opening file %s\n", filename); return; }
    char line[128]; int loadAddr = 4000;
    while(fgets(line, sizeof(line), fp)) {
        char* comment = strchr(line, '/'); if(comment) *comment = '\0';
        char* newline = strchr(line, '\n'); if(newline) *newline = '\0';
        if(strlen(line) < 2) continue;
        char* parts[4]; int partCount = 0;
        char* token = strtok(line, ", \t");
        while(token) { parts[partCount++] = token; token = strtok(NULL, ", \t"); }
        if(partCount == 0) continue;
        Instruction instr; memset(&instr, 0, sizeof(Instruction));
        instr.opcode = get_opcode_enum(parts[0]);
        strcpy(instr.opcodeStr, parts[0]);
        instr.pc = loadAddr;
        instr.rd = -1; instr.rs1 = -1; instr.rs2 = -1;
        switch(instr.opcode) {
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
                instr.rd = parse_reg(parts[1]); instr.rs1 = parse_reg(parts[2]); instr.rs2 = parse_reg(parts[3]); break;
            case OP_ADDL: case OP_SUBL:
                instr.rd = parse_reg(parts[1]); instr.rs1 = parse_reg(parts[2]); instr.imm = parse_imm(parts[3]); break;
            case OP_LOAD:
                instr.rd = parse_reg(parts[1]); instr.rs1 = parse_reg(parts[2]); instr.imm = parse_imm(parts[3]); break;
            case OP_STORE:
                instr.rs1 = parse_reg(parts[1]); instr.rs2 = parse_reg(parts[2]); instr.imm = parse_imm(parts[3]); break;
            case OP_MOVC:
                instr.rd = parse_reg(parts[1]); instr.imm = parse_imm(parts[2]); break;
            case OP_CMP:
                instr.rs1 = parse_reg(parts[1]); instr.rs2 = parse_reg(parts[2]); break;
            case OP_CML:
                instr.rs1 = parse_reg(parts[1]); instr.imm = parse_imm(parts[2]); break;
            case OP_BZ: case OP_BNZ: case OP_BP: case OP_BN:
                instr.imm = parse_imm(parts[1]); break;
            case OP_JUMP:
                instr.rs1 = parse_reg(parts[1]); instr.imm = parse_imm(parts[2]); break;
            case OP_JAL: case OP_JALP:
                instr.rd = parse_reg(parts[1]); instr.imm = parse_imm(parts[2]); break;
            case OP_RET:
                instr.rs1 = parse_reg(parts[1]); break;
            default: break;
        }
        instr.physRd = -1; instr.physRs1 = -1; instr.physRs2 = -1; 
        instr.physCc = -1; instr.physSrcCc = -1;
        instr.robIndex = -1; instr.lsqIndex = -1; instr.bisIndex = -1;
        cpu->codeMemory[(loadAddr - 4000) / 4] = instr;
        loadAddr += 4;
    }
    fclose(fp);
}

void cpu_set_memory(ApexCpu* cpu, int address, int value) {
    if(address >= 0 && address < DATA_MEMORY_SIZE) {
        cpu->dataMemory[address] = value;
        printf("Memory[%d] set to %d\n", address, value);
    }
}

int sets_flags(Opcode op) {
    return (op == OP_ADD || op == OP_SUB || op == OP_AND || op == OP_MUL || 
            op == OP_ADDL || op == OP_SUBL || op == OP_CMP || op == OP_CML);
}
int is_branch(Instruction* i) {
    return (i->opcode == OP_BZ || i->opcode == OP_BNZ || i->opcode == OP_BP || i->opcode == OP_BN ||
            i->opcode == OP_JAL || i->opcode == OP_JALP || i->opcode == OP_RET);
}
int needs_flags(Opcode op) {
    return (op == OP_BZ || op == OP_BNZ || op == OP_BP || op == OP_BN);
}

void update_rs_flags(ApexCpu* cpu, int tag, int val) {
    for(int i=0; i<INT_RS_SIZE; i++) {
        if(cpu->intRs[i].busy && !cpu->intRs[i].instr->flagsReady && cpu->intRs[i].instr->physSrcCc == tag) {
            cpu->intRs[i].instr->flagsValue = val;
            cpu->intRs[i].instr->flagsReady = TRUE;
        }
    }
}
void update_rs_operands(ApexCpu* cpu, int tag, int val) {
    for(int i=0; i<INT_RS_SIZE;i++) {
        if(cpu->intRs[i].busy){
            Instruction* instr = cpu->intRs[i].instr;
            if(!instr->rs1Ready && instr->physRs1 == tag) { instr->rs1Value = val; instr->rs1Ready = TRUE; }
            if(!instr->rs2Ready && instr->physRs2 == tag) { instr->rs2Value = val; instr->rs2Ready = TRUE; }
        }
    }
    for(int i=0; i<MUL_RS_SIZE;i++){
        if(cpu->mulRs[i].busy) {
            Instruction* instr = cpu->mulRs[i].instr;
            if(!instr->rs1Ready && instr->physRs1 == tag) { instr->rs1Value = val; instr->rs1Ready = TRUE; }
            if(!instr->rs2Ready && instr->physRs2 == tag) { instr->rs2Value = val; instr->rs2Ready = TRUE; }
        }
    }
}
void update_lsq_data(ApexCpu* cpu, int tag, int val) {
    for(int i=0; i<LSQ_SIZE; i++) {
        if(cpu->lsq[i].allocated && cpu->lsq[i].instr->opcode == OP_STORE && !cpu->lsq[i].dataValid) {
            if(cpu->lsq[i].instr->physRs1== tag) {
                cpu->lsq[i].storeData= val;
                cpu->lsq[i].dataValid =TRUE;
            }
        }
    }
}

void data_forwarding(ApexCpu* cpu) {
    for(int i=0; i<cpu->forwardingCount; i++) {
        ForwardingData data = cpu->forwardingBuffer[i];
        if(data.isCc) {
            cpu->cprf[data.physRegTag].value= data.value;
            cpu->cprf[data.physRegTag].valid =TRUE;
            update_rs_flags(cpu, data.physRegTag, data.value);
        } else {
            cpu->prf[data.physRegTag].value =data.value;
            cpu->prf[data.physRegTag].valid=TRUE;
            update_rs_operands(cpu, data.physRegTag, data.value);
            update_lsq_data(cpu, data.physRegTag, data.value);
        }
        for(int r=0; r<ROB_SIZE; r++) {
            RobEntry* entry = &cpu->rob[r];
            if(entry->status == 0 && entry->instr) {
                if(data.isCc && entry->physCc==data.physRegTag) entry->status = 1;
                if(!data.isCc && entry->physRd== data.physRegTag) entry->status = 1;
            }
        }
    }
    cpu->forwardingCount = 0;
}

void commitRob(ApexCpu* cpu) {
    if(cpu->robCount == 0) return;
    RobEntry* head = &cpu->rob[cpu->robHead];
    if(head->status == 1){
        // printf("  [COMMIT] %s\n", head->instr->opcodeStr); // Minimal log
        
        if(head->instr->opcode == OP_HALT){
            cpu->simulationHalted = TRUE;
            printf("Simulation Halted by HALT instruction.\n");
            cpu->robCount = 0; cpu->robHead = 0; cpu->robTail = 0;
            return;
        }
        if(head->archRd != -1) {
            cpu->arf[head->archRd] = cpu->prf[head->physRd].value;
            if(head->oldPhysRd != -1) {
                cpu->prf[head->oldPhysRd].allocated = FALSE;
                cpu->prf[head->oldPhysRd].valid = FALSE;
                queue_enqueue(&cpu->freeListPrf, head->oldPhysRd);
            }
        }
        if(head->writesCc) {
            if(head->oldPhysCc != -1) {
                cpu->cprf[head->oldPhysCc].allocated = FALSE;
                cpu->cprf[head->oldPhysCc].valid = FALSE;
                queue_enqueue(&cpu->freeListCprf, head->oldPhysCc);
            }
        }
        if(head->isBranch){
            cpu->bisHead = (cpu->bisHead +1)% BIS_SIZE;
            cpu->bisCount--;
        }
        cpu->instructionsRetired++;
        if(head->instr) free(head->instr);
        memset(head, 0, sizeof(RobEntry));
        head->archRd = -1; head->physRd = -1; head->oldPhysRd = -1;
        head->physCc = -1; head->oldPhysCc = -1;
        cpu->robHead = (cpu->robHead + 1) % ROB_SIZE;
        cpu->robCount--;
    }
}

int is_rob_index_valid(ApexCpu* cpu, int idx) {
    if(cpu->robCount == 0) return FALSE;
    if(cpu->robHead < cpu->robTail) return (idx >= cpu->robHead && idx < cpu->robTail);
    else return (idx >= cpu->robHead || idx < cpu->robTail);
}

void flush_invalid_instructions(ApexCpu* cpu) {
    for(int i=0; i<INT_RS_SIZE; i++) {
        if(cpu->intRs[i].busy && !is_rob_index_valid(cpu, cpu->intRs[i].instr->robIndex)) {
            cpu->intRs[i].busy = FALSE; cpu->intRs[i].instr = NULL;
        }
    }
    for(int i=0; i<MUL_RS_SIZE; i++) {
        if(cpu->mulRs[i].busy && !is_rob_index_valid(cpu, cpu->mulRs[i].instr->robIndex)) {
            cpu->mulRs[i].busy = FALSE; cpu->mulRs[i].instr = NULL;
        }
    }
    for(int i=0; i<LSQ_SIZE; i++) {
        if(cpu->lsq[i].allocated && !is_rob_index_valid(cpu, cpu->lsq[i].instr->robIndex)) {
            cpu->lsq[i].allocated = FALSE; cpu->lsq[i].instr = NULL;
        }
    }
    if(cpu->intFuLatch && !is_rob_index_valid(cpu, cpu->intFuLatch->robIndex)) cpu->intFuLatch = NULL;
    if(cpu->mulFuLatch && !is_rob_index_valid(cpu, cpu->mulFuLatch->robIndex)) cpu->mulFuLatch = NULL;
    for(int i=0; i<3; i++) {
        if(cpu->mulPipeline[i] && !is_rob_index_valid(cpu, cpu->mulPipeline[i]->robIndex)) cpu->mulPipeline[i] = NULL;
    }
    for(int i=0; i<2; i++) {
        if(cpu->mauPipeline[i] && !is_rob_index_valid(cpu, cpu->mauPipeline[i]->robIndex)) cpu->mauPipeline[i] = NULL;
    }
}

void update_btb(ApexCpu* cpu, int pcTag, int target, int taken) {
    int match = -1, lru = -1, empty = -1, minTime = 2147483647;
    for(int i=0; i<8; i++) {
        if(!cpu->btb[i].valid) { if(empty == -1) empty = i; }
        else {
            if(cpu->btb[i].tagPc == pcTag) { match = i; break; }
            if(cpu->btb[i].lruTime < minTime) { minTime = cpu->btb[i].lruTime; lru = i; }
        }
    }
    int idx = (match != -1) ? match : ((empty != -1) ? empty : lru);
    cpu->btb[idx].valid = 1;
    cpu->btb[idx].tagPc = pcTag;
    cpu->btb[idx].targetAddress = target;
    cpu->btb[idx].lruTime = cpu->clock;
    if(taken) { if(cpu->btb[idx].history < 3) cpu->btb[idx].history++; }
    else { if(cpu->btb[idx].history > 0) cpu->btb[idx].history--; }
}

void handle_misprediction(ApexCpu* cpu, Instruction* i) {
    cpu->wasFlushed = TRUE;
    BisEntry* snap = &cpu->bis[i->bisIndex];
    memcpy(cpu->rat, snap->ratSnapshot, sizeof(cpu->rat));
    cpu->ratCc = snap->ratCcSnapshot;
    cpu->freeListPrf = snap->freeListSnapshot;
    cpu->freeListCprf = snap->freeListCcSnapshot;
    cpu->robTail = (snap->robTailSnapshot + 1) % ROB_SIZE;
    if (cpu->robTail >= cpu->robHead) cpu->robCount = cpu->robTail - cpu->robHead;
    else cpu->robCount = ROB_SIZE - (cpu->robHead - cpu->robTail);
    
    if(cpu->fetch1Latch) free(cpu->fetch1Latch); cpu->fetch1Latch= NULL;
    if(cpu->fetch2Latch) free(cpu->fetch2Latch); cpu->fetch2Latch =NULL;
    if(cpu->dispatchLatch) free(cpu->dispatchLatch); cpu->dispatchLatch= NULL;
    
    cpu->forwardingCount = 0; 
    flush_invalid_instructions(cpu);
    
    if(i->opcode== OP_BZ || i->opcode ==OP_BNZ || i->opcode ==OP_BP || i->opcode ==OP_BN) {
        cpu->pc = i->predictedTaken ? (i->pc + 4) : (i->pc + i->imm);
    } else if (i->opcode == OP_JAL || i->opcode == OP_JALP || i->opcode == OP_RET) {
        cpu->pc = i->memoryAddress;
    }
}

void execute_int_fu(ApexCpu* cpu) {
    if(!cpu->intFuLatch) return;
    Instruction* i = cpu->intFuLatch;
    int result = 0; int flags = 0; int genFlags = FALSE; int mispredicted = FALSE;
    
    switch(i->opcode){
        case OP_ADD: case OP_ADDL:
            result = i->rs1Value + ((i->opcode == OP_ADD) ? i->rs2Value : i->imm);
            genFlags = TRUE; break;
        case OP_SUB: case OP_SUBL: case OP_CMP: case OP_CML:
            result = i->rs1Value - ((i->opcode == OP_SUB || i->opcode == OP_CMP) ? i->rs2Value : i->imm);
            genFlags = TRUE; break;
        case OP_MOVC: result = i->imm; break;
        case OP_AND: result = i->rs1Value & i->rs2Value; genFlags = TRUE; break;
        case OP_OR: result = i->rs1Value | i->rs2Value; genFlags = TRUE; break;
        case OP_XOR: result = i->rs1Value ^ i->rs2Value; genFlags = TRUE; break;
        case OP_LOAD: case OP_STORE:
            i->memoryAddress = ((i->opcode == OP_LOAD) ? i->rs1Value : i->rs2Value) + i->imm;
            cpu->lsq[i->lsqIndex].memAddress = i->memoryAddress;
            cpu->lsq[i->lsqIndex].addressValid = TRUE;
            if(i->opcode == OP_STORE) {
                cpu->lsq[i->lsqIndex].storeData = i->rs1Value;
                cpu->lsq[i->lsqIndex].dataValid = TRUE;
            }
            break;
        case OP_BZ: mispredicted = ((i->flagsValue & 1) != 0) ? !i->predictedTaken : i->predictedTaken; break;
        case OP_BNZ: mispredicted = ((i->flagsValue & 1) == 0) ? !i->predictedTaken : i->predictedTaken; break;
        case OP_BP: mispredicted = ((i->flagsValue & 2) != 0) ? !i->predictedTaken : i->predictedTaken; break;
        case OP_BN: mispredicted = ((i->flagsValue & 4) != 0) ? !i->predictedTaken : i->predictedTaken; break;
        case OP_JUMP:
            cpu->pc = i->rs1Value + i->imm;
            cpu->fetch1Latch = NULL; cpu->fetch2Latch = NULL; 
            cpu->fetchStalled = FALSE;
            cpu->wasFlushed = TRUE;
            break;
        case OP_JAL: case OP_JALP:
            result = i->pc + 4;
            i->memoryAddress = (i->opcode == OP_JALP) ? (i->pc + i->imm) : (i->rs1Value + i->imm);
            if(i->memoryAddress != i->predictedTarget) mispredicted = TRUE;
            
            // RUNTIME CHECK
            if (cpu->predictor_enabled) {
                stack_push(&cpu->rap, result);
                if(i->opcode == OP_JAL) update_ctp(cpu, i->pc, i->memoryAddress);
            }
            break;
        case OP_RET:
            i->memoryAddress = i->rs1Value;
            if(i->memoryAddress != i->predictedTarget) mispredicted = TRUE;
            break;
        default: break;
    }
    
    // RUNTIME CHECK
    if (cpu->predictor_enabled) {
        if(i->opcode == OP_BZ || i->opcode == OP_BNZ || i->opcode == OP_BP || i->opcode == OP_BN){
            int taken = !mispredicted ? i->predictedTaken : !i->predictedTaken;
            update_btb(cpu, i->pc, i->memoryAddress, taken);
        }
    }
    
    if(mispredicted && i->bisIndex != -1) handle_misprediction(cpu, i);
    
    if(i->physRd != -1 && i->opcode != OP_LOAD)
        cpu->forwardingBuffer[cpu->forwardingCount++] = (ForwardingData){i->physRd, result, FALSE};
    if(genFlags && i->physCc != -1){
        if(result == 0) flags |= 1;
        if(result > 0) flags |= 2;
        if(result < 0) flags |= 4;
        cpu->forwardingBuffer[cpu->forwardingCount++] = (ForwardingData){i->physCc, flags, TRUE};
    }
    if(i->opcode != OP_LOAD && i->opcode!= OP_STORE) cpu->rob[i->robIndex].status = 1;
    cpu->intFuLatch = NULL;
}

void execute_mul_fu(ApexCpu* cpu) {
    for(int j=2; j>0; j--) cpu->mulPipeline[j] = cpu->mulPipeline[j-1];
    cpu->mulPipeline[0] = cpu->mulFuLatch;
    cpu->mulFuLatch = NULL;
    
    if(cpu->mulPipeline[2]) {
        Instruction* out = cpu->mulPipeline[2];
        int res = out->rs1Value * out->rs2Value;
        cpu->forwardingBuffer[cpu->forwardingCount++] = (ForwardingData){out->physRd, res, FALSE};
        int flags = 0;
        if(res == 0) flags |= 1; else if(res > 0) flags |= 2; else flags |= 4;
        if(out->physCc != -1) cpu->forwardingBuffer[cpu->forwardingCount++] = (ForwardingData){out->physCc, flags, TRUE};
        cpu->rob[out->robIndex].status = 1;
        cpu->mulPipeline[2] = NULL;
    }
}

void execute_mau(ApexCpu* cpu) {
    cpu->mauPipeline[1] = cpu->mauPipeline[0];
    cpu->mauPipeline[0] = NULL;
    if(cpu->mauPipeline[1]) {
        Instruction* out = cpu->mauPipeline[1];
        if(out->opcode == OP_LOAD) {
            int val = cpu->dataMemory[out->memoryAddress];
            cpu->forwardingBuffer[cpu->forwardingCount++] = (ForwardingData){out->physRd, val, FALSE};
        } else {
            cpu->dataMemory[out->memoryAddress] = cpu->lsq[out->lsqIndex].storeData;
        }
        cpu->rob[out->robIndex].status = 1;
        cpu->lsq[out->lsqIndex].allocated = FALSE;
        cpu->lsqHead = (cpu->lsqHead + 1) % LSQ_SIZE;
        cpu->lsqCount--;
    }
    if(cpu->lsqCount > 0 && !cpu->mauPipeline[0]){
        LsqEntry* head = &cpu->lsq[cpu->lsqHead];
        if(head->allocated && cpu->rob[cpu->robHead].instr == head->instr) {
            int loadReady = head->addressValid && head->instr->opcode == OP_LOAD;
            int storeReady = head->addressValid && head->dataValid && head->instr->opcode == OP_STORE;
            if(loadReady || storeReady) cpu->mauPipeline[0] = head->instr;
        }
    }
}

void instructionIssue(ApexCpu* cpu) {
    if(!cpu->intFuLatch) {
        int best = -1, minTime = 2147483647;
        for(int i=0; i<INT_RS_SIZE; i++) {
            if(cpu->intRs[i].busy && cpu->intRs[i].instr->rs1Ready && cpu->intRs[i].instr->rs2Ready) {
                if(needs_flags(cpu->intRs[i].instr->opcode) && !cpu->intRs[i].instr->flagsReady) {
                    int tag = cpu->intRs[i].instr->physSrcCc;
                    if(tag != -1 && cpu->cprf[tag].valid){
                        cpu->intRs[i].instr->flagsValue = cpu->cprf[tag].value;
                        cpu->intRs[i].instr->flagsReady = TRUE;
                    }
                }
                if(!needs_flags(cpu->intRs[i].instr->opcode) || cpu->intRs[i].instr->flagsReady){
                    if(cpu->intRs[i].dispatchTime < minTime) { minTime = cpu->intRs[i].dispatchTime; best = i; }
                }
            }
        }
        if(best!=-1) {
            Instruction* issueInstr = cpu->intRs[best].instr;
            if(issueInstr->physRs1 != -1) issueInstr->rs1Value = cpu->prf[issueInstr->physRs1].value;
            if(issueInstr->physRs2 != -1) issueInstr->rs2Value = cpu->prf[issueInstr->physRs2].value;
            cpu->intFuLatch = issueInstr;
            cpu->intRs[best].busy = FALSE;
        }
    }
    
    if(!cpu->mulFuLatch){
        int bestMul = -1, minMulTime = 2147483647;
        for(int i=0; i<MUL_RS_SIZE; i++) {
            if(cpu->mulRs[i].busy && cpu->mulRs[i].instr->rs1Ready && cpu->mulRs[i].instr->rs2Ready){
                if(cpu->mulRs[i].dispatchTime < minMulTime) { minMulTime = cpu->mulRs[i].dispatchTime; bestMul = i; }
            }
        }
        if(bestMul != -1) {
            Instruction* issueInstr = cpu->mulRs[bestMul].instr;
            if(issueInstr->physRs1 != -1) issueInstr->rs1Value = cpu->prf[issueInstr->physRs1].value;
            if(issueInstr->physRs2 != -1) issueInstr->rs2Value = cpu->prf[issueInstr->physRs2].value;
            cpu->mulFuLatch = issueInstr;
            cpu->mulRs[bestMul].busy = FALSE;
        }
    }
}

void renameSource(ApexCpu* cpu, Instruction* i,int archReg, int opNum){
    if(archReg == -1) {
        if(opNum == 1) i->rs1Ready = TRUE; else i->rs2Ready = TRUE;
        return;
    }
    int phys = cpu->rat[archReg];
    if(phys == -1) {
        int val = cpu->arf[archReg];
        if(opNum == 1) { i->rs1Value = val; i->rs1Ready = TRUE; }
        else { i->rs2Value = val; i->rs2Ready = TRUE; }
    } else {
        if(opNum == 1) i->physRs1 = phys; else i->physRs2 = phys;
        if(cpu->prf[phys].valid) {
            if(opNum == 1) i->rs1Ready = TRUE; else i->rs2Ready = TRUE;
        } else {
            if(opNum == 1) i->rs1Ready = FALSE; else i->rs2Ready = FALSE;
        }
    }
}

void rename_2_dispatch(ApexCpu* cpu){
    if(!cpu->dispatchLatch) return;
    if(cpu->robCount == ROB_SIZE || cpu->lsqCount == LSQ_SIZE) return;
    Instruction* i = cpu->dispatchLatch;
    if(i->opcode == OP_MUL) {
        int full = 1; for(int k=0; k<MUL_RS_SIZE; k++) if(!cpu->mulRs[k].busy) { full = 0; break; }
        if(full) return;
    } else {
        int full = 1; for(int k=0;k<INT_RS_SIZE; k++) if(!cpu->intRs[k].busy) { full = 0; break; }
        if(full) return;
    }
    int robIdx = cpu->robTail;
    cpu->rob[robIdx].instr = i;
    cpu->rob[robIdx].status = 0;
    cpu->rob[robIdx].archRd = i->rd;
    cpu->rob[robIdx].physRd = i->physRd;
    cpu->rob[robIdx].oldPhysRd = (i->rd != -1) ? cpu->rat[i->rd] : -1;
    cpu->rob[robIdx].physCc = i->physCc;
    cpu->rob[robIdx].oldPhysCc = (i->physCc != -1) ?cpu->ratCc : -1;
    if(i->physCc != -1) cpu->rob[robIdx].writesCc= TRUE;
    
    i->robIndex = robIdx;
    cpu->robTail = (cpu->robTail + 1) % ROB_SIZE;
    cpu->robCount++;
    
    renameSource(cpu, i, i->rs1, 1);
    renameSource(cpu, i, i->rs2, 2);
    
    if(is_branch(i)) {
        if(cpu->ratCc != -1) i->physSrcCc = cpu->ratCc;
        if(cpu->ratCc != -1 && cpu->cprf[cpu->ratCc].valid) {
            i->flagsValue = cpu->cprf[cpu->ratCc].value;
            i->flagsReady = TRUE;
        }
    }
    if(i->rd != -1) cpu->rat[i->rd] = i->physRd;
    if(i->physCc != -1) cpu->ratCc = i->physCc;
    
    if(is_branch(i)) {
        BisEntry b;
        b.branchPc = i->pc;
        b.robTailSnapshot = robIdx;
        memcpy(b.ratSnapshot, cpu->rat, sizeof(cpu->rat));
        b.ratCcSnapshot = cpu->ratCc;
        b.freeListSnapshot = cpu->freeListPrf;
        b.freeListCcSnapshot = cpu->freeListCprf;
        cpu->bis[cpu->bisTail] = b;
        i->bisIndex = cpu->bisTail;
        cpu->rob[robIdx].isBranch = TRUE;
        cpu->rob[robIdx].bisIndex = cpu->bisTail;
        cpu->bisTail = (cpu->bisTail + 1) % BIS_SIZE;
        cpu->bisCount++;
    }
    
    cpu->globalDispatchCounter++;
    if(i->opcode == OP_LOAD || i->opcode == OP_STORE) {
        cpu->lsq[cpu->lsqTail].allocated = TRUE;
        cpu->lsq[cpu->lsqTail].instr = i;
        cpu->lsq[cpu->lsqTail].addressValid = FALSE;
        cpu->lsq[cpu->lsqTail].dataValid = FALSE;
        i->lsqIndex = cpu->lsqTail;
        cpu->rob[robIdx].lsqIndex = cpu->lsqTail;
        cpu->lsqTail = (cpu->lsqTail + 1) % LSQ_SIZE;
        cpu->lsqCount++;
        for(int k=0; k<INT_RS_SIZE; k++) {
            if(!cpu->intRs[k].busy) {
                cpu->intRs[k].busy = TRUE; cpu->intRs[k].instr = i;
                cpu->intRs[k].dispatchTime = cpu->globalDispatchCounter;
                break;
            }
        }
    } else if(i->opcode == OP_MUL) {
        for(int k=0; k<MUL_RS_SIZE; k++) {
            if(!cpu->mulRs[k].busy) {
                cpu->mulRs[k].busy = TRUE; cpu->mulRs[k].instr = i;
                cpu->mulRs[k].dispatchTime = cpu->globalDispatchCounter;
                break;
            }
        }
    } else {
        for(int k=0; k<INT_RS_SIZE; k++) {
            if(!cpu->intRs[k].busy) {
                cpu->intRs[k].busy = TRUE; cpu->intRs[k].instr = i;
                cpu->intRs[k].dispatchTime = cpu->globalDispatchCounter;
                break;
            }
        }
    }
    cpu->dispatchLatch = NULL;
}

void decode_rename_1(ApexCpu* cpu) {
    if(!cpu->fetch2Latch) return;
    if(cpu->dispatchLatch) return;
    Instruction* i = cpu->fetch2Latch;
    if(i->opcode == OP_JUMP) {
        cpu->fetchStalled = TRUE;
        if(cpu->fetch1Latch) { free(cpu->fetch1Latch); cpu->fetch1Latch = NULL; }
    }
    
    if(i->rd != -1) {
        if(queue_is_empty(&cpu->freeListPrf)) return;
        int p = queue_dequeue(&cpu->freeListPrf);
        i->physRd = p;
        cpu->prf[p].allocated = TRUE;
        cpu->prf[p].valid = FALSE;
    }
    if(sets_flags(i->opcode)) {
        if(queue_is_empty(&cpu->freeListCprf)) return;
        int c = queue_dequeue(&cpu->freeListCprf);
        i->physCc = c;
        cpu->cprf[c].allocated = TRUE;
        cpu->cprf[c].valid = FALSE;
    }
    cpu->dispatchLatch = i;
    cpu->fetch2Latch = NULL;
}

void fetch_stage_2(ApexCpu* cpu) {
    if(!cpu->fetch1Latch) return;
    if(cpu->fetch2Latch) { cpu->wasStalled = TRUE; return; }
    cpu->fetch2Latch = cpu->fetch1Latch;
    cpu->fetch1Latch = NULL;
}

void fetch_stage_1(ApexCpu* cpu) {
    if(cpu->fetch1Latch || cpu->fetchStalled) { cpu->wasStalled = TRUE; return; }
    if(cpu->simulationHalted) return;
    Instruction* i = create_instruction();
    *i = cpu->codeMemory[(cpu->pc - 4000) / 4];
    
    // RUNTIME CHECK
    if (cpu->predictor_enabled) {
        if(i->opcode == OP_JAL) {
            int predictedTarget = ctp_lookup(cpu, cpu->pc);
            if(predictedTarget != -1) {
                sprintf(i->predictionInfo, "[CTP HIT: Tgt=%d]", predictedTarget);
                i->predictedTarget = predictedTarget;
                cpu->pc = predictedTarget;
                cpu->fetch1Latch = i;
                return;
            } else {
                strcpy(i->predictionInfo, "[CTP MISS]");
            }
        } else if(i->opcode == OP_BZ || i->opcode == OP_BNZ || i->opcode == OP_BP || i->opcode == OP_BN) {
            int match = -1;
            for(int k=0; k<8; k++) {
                if(cpu->btb[k].valid && cpu->btb[k].tagPc == cpu->pc) {
                    cpu->btb[k].lruTime = cpu->clock;
                    match = k; break;
                }
            }
            if(match != -1) {
                sprintf(i->predictionInfo, "[BTB HIT: Tgt=%d Hist=%d]", cpu->btb[match].targetAddress, cpu->btb[match].history);
                if(cpu->btb[match].history >= 2) {
                    i->predictedTaken = TRUE;
                    i->predictedTarget = cpu->btb[match].targetAddress;
                    cpu->pc = cpu->btb[match].targetAddress;
                    cpu->fetch1Latch = i;
                    return;
                }
            } else { strcpy(i->predictionInfo, "[BTB MISS]"); }
        } else if (i->opcode == OP_RET) {
            if(!stack_is_empty(&cpu->rap)) {
                sprintf(i->predictionInfo, "[RAP HIT: Tgt=%d]", stack_peek(&cpu->rap));
                i->predictedTarget = stack_peek(&cpu->rap);
                cpu->pc = stack_pop(&cpu->rap);
                cpu->fetch1Latch = i;
                return;
            } else { strcpy(i->predictionInfo, "[RAP MISS]"); }
        }
    }
    
    cpu->fetch1Latch = i;
    cpu->pc += 4;
}

void print_stage_content(const char* stageName, Instruction* instr) {
    char buffer[128];
    print_instruction_str(instr, buffer);
    if(instr && strlen(instr->predictionInfo) > 0) {
        strcat(buffer, " ");
        strcat(buffer, instr->predictionInfo);
    }
    printf("| %-7s | %-65s |\n", stageName, buffer);
}

void cpu_display(ApexCpu* cpu) {
    cpu_display_all_stages(cpu);
}

void cpu_display_all_stages(ApexCpu* cpu) {
    printf("+-----------------------------------------------------------------------------+\n");
    printf("| Cycle: %-4d | PC: %-5d | Stalled: %s | Flushed: %s | ROB: %2d/%d | LSQ: %d/%d |\n", 
            cpu->clock, cpu->pc, 
            cpu->fetchStalled ? "YES" : "NO ", 
            cpu->wasFlushed ? "YES" : "NO ",
            cpu->robCount, ROB_SIZE,
            cpu->lsqCount, LSQ_SIZE);
    printf("+-----------------------------------------------------------------------------+\n");
    printf("| STAGE   | INSTRUCTION                                                       |\n");
    printf("+-----------------------------------------------------------------------------+\n");
    
    print_stage_content("F1", cpu->fetch1Latch);
    print_stage_content("F2", cpu->fetch2Latch);
    print_stage_content("D1/RN", cpu->dispatchLatch);
    printf("| %-7s | %-65s |\n", "RN2/DIS", (cpu->dispatchLatch) ? "Processing..." : "(Empty)");
    print_stage_content("IntFU", cpu->intFuLatch);
    
    for(int i=0; i<3; i++) {
        char name[10]; sprintf(name, "MulFU-%d", i+1);
        print_stage_content(name, cpu->mulPipeline[i]);
    }
    
    for(int i=0; i<2; i++) {
        char name[10]; sprintf(name, "MemFU-%d", i+1);
        print_stage_content(name, cpu->mauPipeline[i]);
    }
    
    printf("+-----------------------------------------------------------------------------+\n");
    printf("| RENAME TABLE (RAT)                                                          |\n");
    printf("+-----------------------------------------------------------------------------+\n");
    for(int i=0; i<32; i+=8) {
        printf("| ");
        for(int j=i; j<i+8 && j<32; j++) {
            printf("R%02d:P%-2d ", j, cpu->rat[j]);
        }
        printf("|\n");
    }
    printf("| CC-RAT: %-2s                                                                |\n", (cpu->ratCc == -1 ? "-" : "P"));

    printf("+-----------------------------------------------------------------------------+\n");
    printf("| ARCHITECTURAL REGISTER FILE (ARF) - (Partial View R0-R15)                   |\n");
    printf("+-----------------------------------------------------------------------------+\n");
    for(int i=0; i<16; i+=8) {
        printf("| ");
        for(int j=i; j<i+8; j++) {
            printf("R%02d:%-3d ", j, cpu->arf[j]);
        }
        printf(" |\n");
    }

    printf("+-----------------------------------------------------------------------------+\n");
    printf("| RESERVATION STATIONS (Busy Entries)                                         |\n");
    printf("+-----------------------------------------------------------------------------+\n");
    int printedRS = 0;
    for(int i=0; i<INT_RS_SIZE; i++) {
        if(cpu->intRs[i].busy) {
            printf("| IntRS[%d]: %-4s (R1r:%d R2r:%d) -> ROB[%d]                                  |\n", 
                   i, cpu->intRs[i].instr->opcodeStr, 
                   cpu->intRs[i].instr->rs1Ready, cpu->intRs[i].instr->rs2Ready,
                   cpu->intRs[i].instr->robIndex);
            printedRS++;
        }
    }
    for(int i=0; i<MUL_RS_SIZE; i++) {
        if(cpu->mulRs[i].busy) {
            printf("| MulRS[%d]: %-4s (R1r:%d R2r:%d) -> ROB[%d]                                  |\n", 
                   i, cpu->mulRs[i].instr->opcodeStr, 
                   cpu->mulRs[i].instr->rs1Ready, cpu->mulRs[i].instr->rs2Ready,
                   cpu->mulRs[i].instr->robIndex);
            printedRS++;
        }
    }
    if(printedRS == 0) printf("| (All RS Entries Empty)                                                      |\n");
    
    printf("+-----------------------------------------------------------------------------+\n");
    printf("| REORDER BUFFER (Head -> Tail)                                               |\n");
    printf("+-----------------------------------------------------------------------------+\n");
    int count = 0;
    if(cpu->robCount > 0) {
        int curr = cpu->robHead;
        while(count < cpu->robCount) {
             printf("| ROB[%2d]: %-5s Status:%s (ArchRd: R%-2d PhysRd: P%-2d)                   |\n", 
                curr, 
                cpu->rob[curr].instr->opcodeStr,
                cpu->rob[curr].status ? "CMT" : "EXE",
                cpu->rob[curr].archRd, cpu->rob[curr].physRd);
             curr = (curr + 1) % ROB_SIZE;
             count++;
        }
    } else {
        printf("| (Empty)                                                                     |\n");
    }
    
    // RUNTIME CHECK FOR DISPLAY
    if (cpu->predictor_enabled) {
        printf("+-----------------------------------------------------------------------------+\n");
        printf("| PREDICTOR STATE                                                             |\n");
        printf("+-----------------------------------------------------------------------------+\n");
        printf("| RAP Stack: ");
        for(int k = cpu->rap.top; k >= 0; k--) printf("%d ", cpu->rap.items[k]);
        printf("\n| BTB Valid Entries:\n");
        for(int k=0; k<8; k++) {
            if(cpu->btb[k].valid) printf("|  [%d] PC:%d -> Tgt:%d (Hist:%d)\n", k, cpu->btb[k].tagPc, cpu->btb[k].targetAddress, cpu->btb[k].history);
        }
        printf("| CTP Valid Entries:\n");
        int ctpEmpty = 1;
        for(int k=0; k<4; k++) {
            if(cpu->ctp[k].valid) {
                printf("|  [%d] PC:%d -> Tgt:%d\n", k, cpu->ctp[k].tagPc, cpu->ctp[k].targetAddress);
                ctpEmpty = 0;
            }
        }
    }
    
    printf("+-----------------------------------------------------------------------------+\n\n");
}

void cpu_simulate_cycle(ApexCpu* cpu) {
    if (cpu->clock >= MAX_CYCLES) {
        printf("\n*** Max Cycles (%d) Reached. Force Stopping. ***\n", MAX_CYCLES);
        cpu->simulationHalted = 1;
        return;
    }

    if (cpu->simulationHalted && cpu->robCount == 0) return;
    cpu->wasFlushed = FALSE;
    cpu->wasStalled = FALSE;
    data_forwarding(cpu);
    commitRob(cpu);
    execute_mau(cpu);
    execute_mul_fu(cpu);
    execute_int_fu(cpu);
    instructionIssue(cpu);
    rename_2_dispatch(cpu); 
    decode_rename_1(cpu);   
    fetch_stage_2(cpu);     
    fetch_stage_1(cpu);     
    cpu->clock++;
}
/*
 * main.c
 */

#include "apex_cpu.h"

int main(int argc, char* argv[]) {
    // Allow 2 or 3 arguments
    if (argc < 2 || argc > 3) {
        printf("Usage: ./apex_sim <input_file> [predictor_flag]\n");
        printf("Example (Disable Pred): ./apex_sim input.asm\n");
        printf("Example (Enable Pred):  ./apex_sim input.asm 1\n");
        return 1;
    }

    ApexCpu* cpu = (ApexCpu*)malloc(sizeof(ApexCpu));
    cpu_init(cpu);
    cpu_load_program(cpu, argv[1]);
    
    // Check optional argument to enable predictors
    if (argc == 3 && atoi(argv[2]) == 1) {
        cpu->predictor_enabled = TRUE;
        printf("--- PREDICTOR ENABLED ---\n");
    } else {
        cpu->predictor_enabled = FALSE;
        printf("--- PREDICTOR DISABLED ---\n");
    }
    
    char command[64];
    int running = 1;
    
    while(running) {
        if(!fgets(command, sizeof(command), stdin)) break;
        
        command[strcspn(command, "\n")] = 0;
        char* cmd = strtok(command, " ");
        
        if(!cmd) {
            cpu_simulate_cycle(cpu);
            cpu_display(cpu);
            if (cpu->simulationHalted) {
                printf("\n--- Simulation Complete. Exiting CLI. ---\n");
                running = 0;
            }
            continue;
        }
        
        if(!strcmp(cmd, "initialize")) {
            cpu_init(cpu);
            cpu_load_program(cpu, argv[1]);
            // Restore predictor setting after reset
            if (argc == 3 && atoi(argv[2]) == 1) cpu->predictor_enabled = TRUE;
            else cpu->predictor_enabled = FALSE;
            
            printf("System Initialized.\n");
        }
        else if(!strcmp(cmd, "simulate")) {
            char* arg = strtok(NULL, " ");
            int cycles = arg ? atoi(arg) : 1;
            for(int i=0; i<cycles; i++) {
                cpu_simulate_cycle(cpu);
                if (cpu->simulationHalted) break;
            }
            cpu_display(cpu);
            if (cpu->simulationHalted) {
                printf("\n--- Simulation Complete. Exiting CLI. ---\n");
                running = 0;
            }
        }
        else if(!strcmp(cmd, "display")) {
            cpu_display(cpu);
        }
        else if(!strcmp(cmd, "setmem")) {
            char* arg1 = strtok(NULL, " ");
            char* arg2 = strtok(NULL, " ");
            if(arg1 && arg2) {
                cpu_set_memory(cpu, atoi(arg1), atoi(arg2));
            } else if (arg1) {
                FILE* fp = fopen(arg1, "r");
                if(fp) {
                    char line[32];
                    int addr = 0;
                    while(fgets(line, sizeof(line), fp)) {
                       if(strlen(line) > 1) {
                           cpu_set_memory(cpu, addr++, atoi(line));
                       }
                    }
                    fclose(fp);
                    printf("Loaded memory from %s\n", arg1);
                } else {
                    printf("Error: Invalid arguments or file not found.\n");
                }
            }
        }
        else if(!strcmp(cmd, "single_step")) {
            printf("--- Single Step Mode ---\n");
            while(!cpu->simulationHalted) {
                cpu_simulate_cycle(cpu);
                cpu_display_all_stages(cpu);
                printf("Press Enter to advance (or type 'q' to stop)...\n");
                char c[10];
                fgets(c, 10, stdin);
                if(c[0] == 'q' || c[0] == 'Q') break;
            }
            if(cpu->simulationHalted) {
                printf("\n--- Simulation Complete. Exiting CLI. ---\n");
                running = 0; 
            }
        }
        else if(!strcmp(cmd, "exit")) {
            running = 0;
        }
        else {
            cpu_simulate_cycle(cpu);
            cpu_display(cpu);
            if (cpu->simulationHalted) {
                printf("\n--- Simulation Complete. Exiting CLI. ---\n");
                running = 0;
            }
        }
    }
    
    free(cpu);
    return 0;
}
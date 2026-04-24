#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef double (*viospice_jit_func_t)(double t, const double* inputs);

typedef struct JITRegistryNode {
    char* block_id;
    viospice_jit_func_t func_ptr;
    struct JITRegistryNode* next;
} JITRegistryNode;

static JITRegistryNode* jit_registry_head = NULL;

static int jit_id_match(const char* s1, const char* s2) {
    if (!s1 || !s2) return 0;
    
    /* Strip quotes if present for comparison */
    char buf1[256];
    char buf2[256];
    const char *p1 = s1;
    const char *p2 = s2;
    
    if (*p1 == '\"') p1++;
    if (*p2 == '\"') p2++;
    
    strncpy(buf1, p1, 255);
    strncpy(buf2, p2, 255);
    
    char *e1 = buf1 + strlen(buf1) - 1;
    if (e1 >= buf1 && *e1 == '\"') *e1 = '\0';
    
    char *e2 = buf2 + strlen(buf2) - 1;
    if (e2 >= buf2 && *e2 == '\"') *e2 = '\0';
    
    return strcasecmp(buf1, buf2) == 0;
}

__attribute__((visibility("default")))
void ngSpice_RegisterJitLogic(const char* block_id, viospice_jit_func_t func_ptr) {
    JITRegistryNode* current = jit_registry_head;
    while (current != NULL) {
        if (jit_id_match(current->block_id, block_id)) {
            current->func_ptr = func_ptr;
            return;
        }
        current = current->next;
    }
    JITRegistryNode* new_node = (JITRegistryNode*)malloc(sizeof(JITRegistryNode));
    if (new_node) {
        new_node->block_id = strdup(block_id);
        new_node->func_ptr = func_ptr;
        new_node->next = jit_registry_head;
        jit_registry_head = new_node;
    }
}

__attribute__((visibility("default")))
viospice_jit_func_t viospice_jit_lookup(const char* block_id) {
    JITRegistryNode* current = jit_registry_head;
    while (current != NULL) {
        if (jit_id_match(current->block_id, block_id)) {
            return current->func_ptr;
        }
        current = current->next;
    }
    return NULL;
}

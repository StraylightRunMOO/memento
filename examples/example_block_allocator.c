/*
 * Block allocator example
 * 
 * This example demonstrates the block allocator, which is optimized for
 * allocations of similar sizes with good temporal locality and memory recycling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

/* Simulate a particle system that allocates many small objects */
typedef struct {
    float x, y, z;          /* Position */
    float vx, vy, vz;       /* Velocity */
    float r, g, b, a;       /* Color (RGBA) */
    float size;             /* Particle size */
    int lifetime;           /* Remaining lifetime */
} Particle;

static void simulate_particle_system(memento_allocator_t* allocator, int num_particles) {
    printf("  Simulating particle system with %d particles...\n", num_particles);
    
    Particle** particles = malloc(num_particles * sizeof(Particle*));
    if (!particles) {
        printf("    Failed to allocate particle array!\n");
        return;
    }
    
    /* Create particles */
    printf("    Creating particles...\n");
    for (int i = 0; i < num_particles; i++) {
        memento_result_t result = memento_alloc(allocator, sizeof(Particle));
        if (!result.success) {
            printf("      Failed to allocate particle %d!\n", i);
            /* Clean up already allocated particles */
            for (int j = 0; j < i; j++) {
                memento_free(allocator, particles[j]);
            }
            free(particles);
            return;
        }
        
        particles[i] = (Particle*)result.ptr;
        
        /* Initialize particle */
        particles[i]->x = (float)(rand() % 100 - 50);
        particles[i]->y = (float)(rand() % 100 - 50);
        particles[i]->z = (float)(rand() % 100 - 50);
        
        particles[i]->vx = (float)(rand() % 20 - 10) / 10.0f;
        particles[i]->vy = (float)(rand() % 20 - 10) / 10.0f;
        particles[i]->vz = (float)(rand() % 20 - 10) / 10.0f;
        
        particles[i]->r = (float)(rand() % 100) / 100.0f;
        particles[i]->g = (float)(rand() % 100) / 100.0f;
        particles[i]->b = (float)(rand() % 100) / 100.0f;
        particles[i]->a = 1.0f;
        
        particles[i]->size = (float)(rand() % 50 + 10) / 10.0f;
        particles[i]->lifetime = rand() % 100 + 50;
    }
    
    /* Simulate particle updates */
    printf("    Updating particles for 10 frames...\n");
    for (int frame = 0; frame < 10; frame++) {
        int active_count = 0;
        
        for (int i = 0; i < num_particles; i++) {
            if (particles[i] && particles[i]->lifetime > 0) {
                /* Update position */
                particles[i]->x += particles[i]->vx;
                particles[i]->y += particles[i]->vy;
                particles[i]->z += particles[i]->vz;
                
                /* Update lifetime */
                particles[i]->lifetime--;
                
                /* Fade out */
                particles[i]->a = (float)particles[i]->lifetime / 150.0f;
                
                active_count++;
            }
        }
        
        if (frame % 3 == 0) {
            printf("      Frame %d: %d particles active\n", frame, active_count);
        }
    }
    
    /* Remove dead particles */
    printf("    Removing dead particles...\n");
    int removed = 0;
    for (int i = 0; i < num_particles; i++) {
        if (particles[i] && particles[i]->lifetime <= 0) {
            memento_free(allocator, particles[i]);
            particles[i] = NULL;
            removed++;
        }
    }
    printf("      Removed %d dead particles\n", removed);
    
    /* Create new particles to replace dead ones */
    printf("    Creating new particles...\n");
    int created = 0;
    for (int i = 0; i < num_particles; i++) {
        if (particles[i] == NULL) {
            memento_result_t result = memento_alloc(allocator, sizeof(Particle));
            if (result.success) {
                particles[i] = (Particle*)result.ptr;
                /* Initialize new particle (simplified) */
                particles[i]->x = 0.0f;
                particles[i]->y = 0.0f;
                particles[i]->z = 0.0f;
                particles[i]->lifetime = rand() % 100 + 50;
                created++;
            }
        }
    }
    printf("      Created %d new particles\n", created);
    
    /* Clean up remaining particles */
    printf("    Cleaning up remaining particles...\n");
    for (int i = 0; i < num_particles; i++) {
        if (particles[i]) {
            memento_free(allocator, particles[i]);
        }
    }
    
    free(particles);
}

int main(void) {
    printf("Memento Block Allocator Example\n");
    printf("===============================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("1. Creating backing allocator...\n");
    memento_allocator_t* backing = memento_create_thread_cache("block_backing");
    if (!backing) {
        printf("Failed to create backing allocator!\n");
        memento_shutdown();
        return 1;
    }
    
    printf("2. Creating block allocator...\n");
    memento_allocator_t* block_allocator = memento_create_block_allocator("particle_system", backing);
    if (!block_allocator) {
        printf("Failed to create block allocator!\n");
        memento_destroy_allocator(backing);
        memento_shutdown();
        return 1;
    }
    
    printf("   Block allocator created: %s\n", block_allocator->name);
    printf("   Block size: %d bytes\n", MEMENTO_BLOCK_SIZE);
    printf("   Recycler slots: %d\n\n", MEMENTO_RECYCLE_SLOTS);
    
    /* Demonstrate block allocator with particle system */
    printf("3. Running particle system simulation...\n");
    simulate_particle_system(block_allocator, 100);
    
    /* Show block allocator statistics */
    printf("\n4. Block allocator statistics:\n");
    const memento_stats_t* stats = memento_get_stats(block_allocator);
    if (stats) {
        printf("   Block allocations: %zu\n", stats->block_allocations);
        printf("   Block deallocations: %zu\n", stats->block_deallocations);
        printf("   Total allocated: %zu bytes\n", stats->total_allocated);
        printf("   Current usage: %zu bytes\n", stats->current_usage);
        printf("   Peak usage: %zu bytes\n", stats->peak_usage);
    }
    
    /* Demonstrate memory recycling */
    printf("\n5. Demonstrating memory recycling...\n");
    printf("   Allocating and freeing the same size repeatedly...\n");
    
    size_t test_size = sizeof(Particle);
    void* ptrs[10];
    
    /* First allocation wave */
    for (int i = 0; i < 10; i++) {
        memento_result_t result = memento_alloc(block_allocator, test_size);
        if (result.success) {
            ptrs[i] = result.ptr;
            printf("     First wave: allocated at %p\n", result.ptr);
        }
    }
    
    /* Free all allocations */
    for (int i = 0; i < 10; i++) {
        memento_free(block_allocator, ptrs[i]);
    }
    
    /* Second allocation wave - should reuse freed memory */
    printf("   Second allocation wave (should reuse memory):\n");
    for (int i = 0; i < 10; i++) {
        memento_result_t result = memento_alloc(block_allocator, test_size);
        if (result.success) {
            printf("     Second wave: allocated at %p\n", result.ptr);
            memento_free(block_allocator, result.ptr);
        }
    }
    
    /* Clean up */
    printf("\n6. Cleaning up...\n");
    memento_destroy_allocator(block_allocator);
    memento_destroy_allocator(backing);
    memento_shutdown();
    
    printf("\n✓ Block allocator example completed successfully!\n");
    return 0;
}
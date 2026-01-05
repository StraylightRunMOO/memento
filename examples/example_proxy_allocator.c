/*
 * Proxy allocator example
 * 
 * This example demonstrates the proxy allocator, which wraps other allocators
 * to add statistics tracking, debugging capabilities, and hierarchical memory management.
 */

#include <stdio.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

/* Simulate different game subsystems */
static void graphics_subsystem(memento_allocator_t* allocator) {
    printf("  Graphics subsystem:\n");
    
    /* Allocate vertex data */
    struct Vertex {
        float x, y, z;
        float u, v;
        uint32_t color;
    };
    
    memento_result_t result = memento_alloc(allocator, sizeof(struct Vertex) * 1000);
    if (result.success) {
        struct Vertex* vertices = (struct Vertex*)result.ptr;
        printf("    Allocated %zu bytes for vertex data at %p\n", result.size, (void*)vertices);
        
        /* Initialize some vertex data */
        for (int i = 0; i < 1000; i++) {
            vertices[i].x = (float)(i % 100);
            vertices[i].y = (float)(i / 100);
            vertices[i].z = 0.0f;
            vertices[i].color = 0xFFFFFFFF;
        }
        
        memento_free(allocator, vertices);
        printf("    Freed vertex data\n");
    }
}

static void physics_subsystem(memento_allocator_t* allocator) {
    printf("  Physics subsystem:\n");
    
    /* Allocate physics bodies */
    struct PhysicsBody {
        float position[3];
        float velocity[3];
        float mass;
        int active;
    };
    
    for (int i = 0; i < 5; i++) {
        memento_result_t result = memento_alloc(allocator, sizeof(struct PhysicsBody));
        if (result.success) {
            struct PhysicsBody* body = (struct PhysicsBody*)result.ptr;
            printf("    Created physics body %d at %p\n", i, (void*)body);
            
            body->position[0] = (float)(i * 10);
            body->position[1] = 0.0f;
            body->position[2] = 0.0f;
            body->mass = 1.0f + i;
            body->active = 1;
            
            memento_free(allocator, body);
            printf("    Destroyed physics body %d\n", i);
        }
    }
}

static void audio_subsystem(memento_allocator_t* allocator) {
    printf("  Audio subsystem:\n");
    
    /* Allocate audio samples */
    struct AudioSample {
        float* data;
        size_t length;
        int channels;
        int sample_rate;
    };
    
    memento_result_t result = memento_alloc(allocator, sizeof(struct AudioSample));
    if (result.success) {
        struct AudioSample* sample = (struct AudioSample*)result.ptr;
        printf("    Created audio sample at %p\n", (void*)sample);
        
        sample->length = 44100; /* 1 second at 44.1kHz */
        sample->channels = 2;
        sample->sample_rate = 44100;
        
        /* Allocate sample data */
        memento_result_t data_result = memento_alloc(allocator, sample->length * sizeof(float));
        if (data_result.success) {
            sample->data = (float*)data_result.ptr;
            printf("    Allocated %zu bytes for audio data\n", data_result.size);
            
            /* Initialize audio data */
            for (size_t i = 0; i < sample->length; i++) {
                sample->data[i] = 0.0f;
            }
            
            memento_free(allocator, sample->data);
        }
        
        memento_free(allocator, sample);
        printf("    Destroyed audio sample\n");
    }
}

int main(void) {
    printf("Memento Proxy Allocator Example\n");
    printf("===============================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("1. Creating root allocator...\n");
    memento_allocator_t* root = memento_create_thread_cache("root");
    if (!root) {
        printf("Failed to create root allocator!\n");
        memento_shutdown();
        return 1;
    }
    
    printf("2. Creating proxy allocators for subsystems...\n");
    memento_allocator_t* graphics_proxy = memento_create_proxy_allocator("graphics", root);
    memento_allocator_t* physics_proxy = memento_create_proxy_allocator("physics", root);
    memento_allocator_t* audio_proxy = memento_create_proxy_allocator("audio", root);
    
    if (!graphics_proxy || !physics_proxy || !audio_proxy) {
        printf("Failed to create proxy allocators!\n");
        memento_destroy_allocator(root);
        memento_shutdown();
        return 1;
    }
    
    printf("   Created proxy allocators:\n");
    printf("     Graphics: %s\n", graphics_proxy->name);
    printf("     Physics: %s\n", physics_proxy->name);
    printf("     Audio: %s\n", audio_proxy->name);
    
    /* Show initial statistics */
    printf("\n3. Initial statistics:\n");
    const memento_stats_t* root_stats = memento_get_stats(root);
    printf("   Root allocator: %zu allocations, %zu bytes allocated\n", 
           root_stats->allocation_count, root_stats->total_allocated);
    
    /* Simulate game subsystems */
    printf("\n4. Simulating game subsystems...\n");
    graphics_subsystem(graphics_proxy);
    physics_subsystem(physics_proxy);
    audio_subsystem(audio_proxy);
    
    /* Show final statistics */
    printf("\n5. Final statistics:\n");
    const memento_stats_t* graphics_stats = memento_get_stats(graphics_proxy);
    const memento_stats_t* physics_stats = memento_get_stats(physics_proxy);
    const memento_stats_t* audio_stats = memento_get_stats(audio_proxy);
    const memento_stats_t* final_root_stats = memento_get_stats(root);
    
    printf("   Graphics proxy: %zu allocations, %zu bytes allocated\n",
           graphics_stats->allocation_count, graphics_stats->total_allocated);
    printf("   Physics proxy: %zu allocations, %zu bytes allocated\n",
           physics_stats->allocation_count, physics_stats->total_allocated);
    printf("   Audio proxy: %zu allocations, %zu bytes allocated\n",
           audio_stats->allocation_count, audio_stats->total_allocated);
    printf("   Root allocator: %zu allocations, %zu bytes allocated\n",
           final_root_stats->allocation_count, final_root_stats->total_allocated);
    
    /* Clean up */
    printf("\n6. Cleaning up...\n");
    printf("   Destroying proxy allocators...\n");
    memento_destroy_allocator(audio_proxy);
    memento_destroy_allocator(physics_proxy);
    memento_destroy_allocator(graphics_proxy);
    printf("   Destroying root allocator...\n");
    memento_destroy_allocator(root);
    printf("   Shutting down memento...\n");
    memento_shutdown();
    
    printf("\n✓ Proxy allocator example completed successfully!\n");
    return 0;
}

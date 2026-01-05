/*
 * Stack allocator example
 * 
 * This example demonstrates the stack allocator, which is optimized for
 * temporary allocations that follow a LIFO (Last In, First Out) pattern.
 */

#include <stdio.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

/* Simulate a rendering system that needs temporary allocations */
typedef struct {
    float x, y, z;
    float r, g, b, a;
} Vertex;

typedef struct {
    Vertex* vertices;
    size_t vertex_count;
    uint32_t* indices;
    size_t index_count;
} Mesh;

static Mesh* create_mesh(memento_allocator_t* allocator, size_t vertex_count, size_t index_count) {
    printf("  Creating mesh with %zu vertices and %zu indices...\n", vertex_count, index_count);
    
    /* Allocate mesh structure */
    memento_result_t mesh_result = memento_alloc(allocator, sizeof(Mesh));
    if (!mesh_result.success) {
        printf("    Failed to allocate mesh structure!\n");
        return NULL;
    }
    
    Mesh* mesh = (Mesh*)mesh_result.ptr;
    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;
    
    /* Allocate vertex buffer */
    memento_result_t vertex_result = memento_alloc(allocator, vertex_count * sizeof(Vertex));
    if (!vertex_result.success) {
        printf("    Failed to allocate vertex buffer!\n");
        memento_free(allocator, mesh);
        return NULL;
    }
    
    mesh->vertices = (Vertex*)vertex_result.ptr;
    
    /* Allocate index buffer */
    memento_result_t index_result = memento_alloc(allocator, index_count * sizeof(uint32_t));
    if (!index_result.success) {
        printf("    Failed to allocate index buffer!\n");
        memento_free(allocator, mesh->vertices);
        memento_free(allocator, mesh);
        return NULL;
    }
    
    mesh->indices = (uint32_t*)index_result.ptr;
    
    /* Initialize mesh data */
    for (size_t i = 0; i < vertex_count; i++) {
        mesh->vertices[i].x = (float)(i % 10);
        mesh->vertices[i].y = (float)(i / 10);
        mesh->vertices[i].z = 0.0f;
        mesh->vertices[i].r = 1.0f;
        mesh->vertices[i].g = 0.0f;
        mesh->vertices[i].b = 0.0f;
        mesh->vertices[i].a = 1.0f;
    }
    
    for (size_t i = 0; i < index_count; i++) {
        mesh->indices[i] = (uint32_t)(i % vertex_count);
    }
    
    printf("    Mesh created at %p\n", (void*)mesh);
    return mesh;
}

static void render_mesh(const Mesh* mesh) {
    printf("  Rendering mesh:\n");
    printf("    Vertices: %zu\n", mesh->vertex_count);
    printf("    Indices: %zu\n", mesh->index_count);
    printf("    First vertex: (%.1f, %.1f, %.1f)\n", 
           mesh->vertices[0].x, mesh->vertices[0].y, mesh->vertices[0].z);
    printf("    Last index: %u\n", mesh->indices[mesh->index_count - 1]);
}

static void simulate_frame_rendering(memento_allocator_t* stack_allocator) {
    printf("  Simulating frame rendering...\n");
    
    /* Allocate temporary vertex transformation buffer */
    size_t transform_buffer_size = 1000 * sizeof(float) * 4; /* 1000 vertices * 4 components */
    memento_result_t transform_result = memento_alloc(stack_allocator, transform_buffer_size);
    if (transform_result.success) {
        float* transform_buffer = (float*)transform_result.ptr;
        printf("    Allocated %zu bytes for vertex transformations\n", transform_buffer_size);
        
        /* Initialize transformation buffer */
        for (size_t i = 0; i < 1000 * 4; i++) {
            transform_buffer[i] = 1.0f; /* Identity transformation */
        }
        
        /* Note: In a real stack allocator, we wouldn't free individual allocations */
        /* but since this is a demonstration, we'll show the allocation working */
        printf("    Using transformation buffer...\n");
        
        /* Stack allocator doesn't support individual deallocation */
        /* The entire stack will be cleared at once later */
    }
    
    /* Allocate temporary lighting data */
    struct LightData {
        float position[3];
        float color[3];
        float intensity;
    };
    
    size_t light_count = 8;
    memento_result_t light_result = memento_alloc(stack_allocator, light_count * sizeof(struct LightData));
    if (light_result.success) {
        struct LightData* lights = (struct LightData*)light_result.ptr;
        printf("    Allocated space for %zu lights\n", light_count);
        
        /* Initialize light data */
        for (size_t i = 0; i < light_count; i++) {
            lights[i].position[0] = (float)i * 10.0f;
            lights[i].position[1] = 10.0f;
            lights[i].position[2] = 0.0f;
            lights[i].color[0] = 1.0f;
            lights[i].color[1] = 1.0f;
            lights[i].color[2] = 1.0f;
            lights[i].intensity = 1.0f;
        }
        printf("    Initialized light data\n");
    }
}

int main(void) {
    printf("Memento Stack Allocator Example\n");
    printf("===============================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("1. Creating backing allocator...\n");
    memento_allocator_t* backing = memento_create_thread_cache("stack_backing");
    if (!backing) {
        printf("Failed to create backing allocator!\n");
        memento_shutdown();
        return 1;
    }
    
    printf("2. Creating stack allocator with 64KB capacity...\n");
    memento_allocator_t* stack = memento_create_stack_allocator("render_stack", 64 * 1024, backing);
    if (!stack) {
        printf("Failed to create stack allocator!\n");
        memento_destroy_allocator(backing);
        memento_shutdown();
        return 1;
    }
    
    printf("   Stack allocator created: %s\n", stack->name);
    printf("   Capacity: 64KB\n");
    printf("   Note: Stack allocator doesn't support individual deallocation\n\n");
    
    /* Create some meshes */
    printf("3. Creating meshes...\n");
    Mesh* mesh1 = create_mesh(stack, 100, 300);
    Mesh* mesh2 = create_mesh(stack, 200, 600);
    
    if (mesh1) render_mesh(mesh1);
    if (mesh2) render_mesh(mesh2);
    
    /* Simulate frame rendering (temporary allocations) */
    printf("\n4. Simulating frame rendering with temporary allocations...\n");
    simulate_frame_rendering(stack);
    
    /* Create more meshes (stack will be reused) */
    printf("\n5. Creating more meshes (stack space reused)...\n");
    Mesh* mesh3 = create_mesh(stack, 150, 450);
    if (mesh3) render_mesh(mesh3);
    
    /* Show statistics */
    printf("\n6. Allocator statistics:\n");
    const memento_stats_t* stack_stats = memento_get_stats(stack);
    const memento_stats_t* backing_stats = memento_get_stats(backing);
    
    printf("   Stack allocator:\n");
    printf("     Allocations: %zu\n", stack_stats->allocation_count);
    printf("     Total allocated: %zu bytes\n", stack_stats->total_allocated);
    printf("     Current usage: %zu bytes\n", stack_stats->current_usage);
    printf("   Backing allocator:\n");
    printf("     Allocations: %zu\n", backing_stats->allocation_count);
    printf("     Total allocated: %zu bytes\n", backing_stats->total_allocated);
    
    /* Clean up */
    printf("\n7. Cleaning up...\n");
    printf("   Note: All stack allocations will be freed when stack is destroyed\n");
    memento_destroy_allocator(stack);
    memento_destroy_allocator(backing);
    memento_shutdown();
    
    printf("\n✓ Stack allocator example completed successfully!\n");
    return 0;
}

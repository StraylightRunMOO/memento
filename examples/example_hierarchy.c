/*
 * Hierarchical allocator example
 * 
 * This example demonstrates how to create a hierarchy of allocators
 * for different subsystems, providing memory tracking and debugging
 * capabilities for complex applications.
 */

#include <stdio.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

/* Simulate different application layers */
static void application_layer(memento_allocator_t* app_allocator) {
    printf("  Application Layer:\n");
    
    /* Application-level data structures */
    struct AppConfig {
        char app_name[64];
        int version;
        int debug_mode;
        void* user_data;
    };
    
    memento_result_t result = memento_alloc(app_allocator, sizeof(struct AppConfig));
    if (result.success) {
        struct AppConfig* config = (struct AppConfig*)result.ptr;
        printf("    Created app config at %p\n", (void*)config);
        
        strcpy(config->app_name, "MyApplication");
        config->version = 100;
        config->debug_mode = 1;
        config->user_data = NULL;
        
        memento_free(app_allocator, config);
        printf("    Destroyed app config\n");
    }
}

static void game_engine_layer(memento_allocator_t* engine_allocator) {
    printf("  Game Engine Layer:\n");
    
    /* Engine-level systems */
    struct GameState {
        int current_level;
        int player_health;
        int score;
        void* level_data;
    };
    
    memento_result_t result = memento_alloc(engine_allocator, sizeof(struct GameState));
    if (result.success) {
        struct GameState* state = (struct GameState*)result.ptr;
        printf("    Created game state at %p\n", (void*)state);
        
        state->current_level = 1;
        state->player_health = 100;
        state->score = 0;
        state->level_data = NULL;
        
        memento_free(engine_allocator, state);
        printf("    Destroyed game state\n");
    }
}

static void rendering_layer(memento_allocator_t* render_allocator) {
    printf("  Rendering Layer:\n");
    
    /* Rendering resources */
    struct RenderResources {
        void* vertex_buffers[4];
        void* index_buffers[2];
        void* texture_data[8];
        int buffer_count;
        int texture_count;
    };
    
    memento_result_t result = memento_alloc(render_allocator, sizeof(struct RenderResources));
    if (result.success) {
        struct RenderResources* resources = (struct RenderResources*)result.ptr;
        printf("    Created render resources at %p\n", (void*)resources);
        
        resources->buffer_count = 0;
        resources->texture_count = 0;
        
        /* Allocate some buffers */
        for (int i = 0; i < 2; i++) {
            memento_result_t buffer_result = memento_alloc(render_allocator, 1024);
            if (buffer_result.success) {
                resources->vertex_buffers[i] = buffer_result.ptr;
                resources->buffer_count++;
                printf("      Allocated vertex buffer %d\n", i);
            }
        }
        
        memento_free(render_allocator, resources);
        printf("    Destroyed render resources\n");
    }
}

static void ui_layer(memento_allocator_t* ui_allocator) {
    printf("  UI Layer:\n");
    
    /* UI elements */
    struct UIElement {
        char type[32];
        float position[2];
        float size[2];
        int visible;
        void* children;
    };
    
    /* Create UI elements */
    const char* element_types[] = {"button", "text", "image", "panel"};
    
    for (int i = 0; i < 4; i++) {
        memento_result_t result = memento_alloc(ui_allocator, sizeof(struct UIElement));
        if (result.success) {
            struct UIElement* element = (struct UIElement*)result.ptr;
            printf("    Created UI element %d at %p\n", i, (void*)element);
            
            strcpy(element->type, element_types[i]);
            element->position[0] = (float)(i * 100);
            element->position[1] = (float)(i * 50);
            element->size[0] = 80.0f;
            element->size[1] = 30.0f;
            element->visible = 1;
            element->children = NULL;
            
            memento_free(ui_allocator, element);
            printf("    Destroyed UI element %d\n", i);
        }
    }
}

int main(void) {
    printf("Memento Hierarchical Allocator Example\n");
    printf("=====================================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("1. Creating root allocator...\n");
    memento_allocator_t* root = memento_create_thread_cache("hierarchy_root");
    if (!root) {
        printf("Failed to create root allocator!\n");
        memento_shutdown();
        return 1;
    }
    
    printf("2. Creating hierarchical allocators...\n");
    printf("   Structure:\n");
    printf("     Root (thread cache)\n");
    printf("     ├── Application (proxy)\n");
    printf("     ├── Engine (proxy)\n");
    printf("     ├── Rendering (proxy)\n");
    printf("     └── UI (proxy)\n\n");
    
    /* Create hierarchical allocators */
    memento_allocator_t* app_proxy = memento_create_proxy_allocator("application", root);
    memento_allocator_t* engine_proxy = memento_create_proxy_allocator("engine", root);
    memento_allocator_t* render_proxy = memento_create_proxy_allocator("rendering", root);
    memento_allocator_t* ui_proxy = memento_create_proxy_allocator("ui", root);
    
    if (!app_proxy || !engine_proxy || !render_proxy || !ui_proxy) {
        printf("Failed to create proxy allocators!\n");
        memento_destroy_allocator(root);
        memento_shutdown();
        return 1;
    }
    
    /* Simulate application operation */
    printf("3. Simulating application operation...\n");
    application_layer(app_proxy);
    game_engine_layer(engine_proxy);
    rendering_layer(render_proxy);
    ui_layer(ui_proxy);
    
    /* Show hierarchical statistics */
    printf("\n4. Hierarchical memory statistics:\n");
    const memento_stats_t* root_stats = memento_get_stats(root);
    const memento_stats_t* app_stats = memento_get_stats(app_proxy);
    const memento_stats_t* engine_stats = memento_get_stats(engine_proxy);
    const memento_stats_t* render_stats = memento_get_stats(render_proxy);
    const memento_stats_t* ui_stats = memento_get_stats(ui_proxy);
    
    printf("   Root (total): %zu allocations, %zu bytes\n",
           root_stats->allocation_count, root_stats->total_allocated);
    printf("   Application: %zu allocations, %zu bytes\n",
           app_stats->allocation_count, app_stats->total_allocated);
    printf("   Engine: %zu allocations, %zu bytes\n",
           engine_stats->allocation_count, engine_stats->total_allocated);
    printf("   Rendering: %zu allocations, %zu bytes\n",
           render_stats->allocation_count, render_stats->total_allocated);
    printf("   UI: %zu allocations, %zu bytes\n",
           ui_stats->allocation_count, ui_stats->total_allocated);
    
    /* Verify hierarchy integrity */
    printf("\n5. Verifying hierarchy integrity:\n");
    size_t proxy_total = app_stats->allocation_count + engine_stats->allocation_count + 
                        render_stats->allocation_count + ui_stats->allocation_count;
    printf("   Proxy allocations: %zu\n", proxy_total);
    printf("   Root allocations: %zu\n", root_stats->allocation_count);
    printf("   Hierarchy is %s\n", 
           proxy_total <= root_stats->allocation_count ? "consistent" : "inconsistent");
    
    /* Clean up */
    printf("\n6. Cleaning up...\n");
    printf("   Destroying proxy allocators...\n");
    memento_destroy_allocator(ui_proxy);
    memento_destroy_allocator(render_proxy);
    memento_destroy_allocator(engine_proxy);
    memento_destroy_allocator(app_proxy);
    printf("   Destroying root allocator...\n");
    memento_destroy_allocator(root);
    printf("   Shutting down memento...\n");
    memento_shutdown();
    
    printf("\n✓ Hierarchical allocator example completed successfully!\n");
    return 0;
}

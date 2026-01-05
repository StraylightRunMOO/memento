/*
 * Game Engine Memory Management Example
 * 
 * This example demonstrates how to use memento in a real game engine
 * with different subsystems, hierarchical allocators, and proper cleanup.
 */

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <random>
#include <chrono>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

// Forward declarations
class GameEngine;
class Entity;

// Base component class
class Component {
public:
    virtual ~Component() = default;
    virtual void update(float /* delta_time */) {}
    virtual void print() const = 0;
};

// Component types
class TransformComponent : public Component {
private:
    float position_[3];
    float rotation_[3];
    float scale_[3];
    
public:
    TransformComponent(float x, float y, float z) {
        position_[0] = x; position_[1] = y; position_[2] = z;
        rotation_[0] = rotation_[1] = rotation_[2] = 0.0f;
        scale_[0] = scale_[1] = scale_[2] = 1.0f;
    }
    
    void setPosition(float x, float y, float z) {
        position_[0] = x; position_[1] = y; position_[2] = z;
    }
    
    void move(float dx, float dy, float dz) {
        position_[0] += dx; position_[1] += dy; position_[2] += dz;
    }
    
    void print() const {
        std::cout << "      Pos: (" << position_[0] << ", " << position_[1] << ", " << position_[2] << ")\n";
        std::cout << "      Rot: (" << rotation_[0] << ", " << rotation_[1] << ", " << rotation_[2] << ")\n";
        std::cout << "      Scale: (" << scale_[0] << ", " << scale_[1] << ", " << scale_[2] << ")\n";
    }
};

class MeshComponent : public Component {
private:
    std::string mesh_name_;
    std::string material_name_;
    bool visible_;
    
public:
    MeshComponent(const std::string& mesh, const std::string& material)
        : mesh_name_(mesh), material_name_(material), visible_(true) {}
    
    void setVisible(bool visible) { visible_ = visible; }
    bool isVisible() const { return visible_; }
    
    void print() const {
        std::cout << "      Mesh: " << mesh_name_ << ", Material: " << material_name_ 
                  << ", Visible: " << (visible_ ? "true" : "false") << "\n";
    }
};

class PhysicsComponent : public Component {
private:
    float velocity_[3];
    float mass_;
    bool affected_by_gravity_;
    
public:
    PhysicsComponent(float mass, bool gravity = true) 
        : mass_(mass), affected_by_gravity_(gravity) {
        velocity_[0] = velocity_[1] = velocity_[2] = 0.0f;
    }
    
    void setVelocity(float vx, float vy, float vz) {
        velocity_[0] = vx; velocity_[1] = vy; velocity_[2] = vz;
    }
    
    void applyForce(float fx, float fy, float fz) {
        velocity_[0] += fx / mass_;
        velocity_[1] += fy / mass_;
        velocity_[2] += fz / mass_;
    }
    
    void update(float delta_time) {
        if (affected_by_gravity_) {
            velocity_[1] -= 9.81f * delta_time;  // Simple gravity
        }
    }
    
    void print() const {
        std::cout << "      Velocity: (" << velocity_[0] << ", " << velocity_[1] << ", " << velocity_[2] << ")\n";
        std::cout << "      Mass: " << mass_ << ", Gravity: " << (affected_by_gravity_ ? "true" : "false") << "\n";
    }
};

// Entity class that uses memento allocation
class Entity {
private:
    int id_;
    std::string name_;
    std::vector<memento::unique_ptr<Component>> components_;
    memento::allocator* allocator_;
    
public:
    Entity(int id, const std::string& name, memento::allocator* alloc)
        : id_(id), name_(name), allocator_(alloc) {
        std::cout << "    Entity created: " << name_ << " (ID: " << id_ << ")\n";
    }
    
    ~Entity() {
        std::cout << "    Entity destroyed: " << name_ << " (ID: " << id_ << ")\n";
    }
    
    int getId() const { return id_; }
    const std::string& getName() const { return name_; }
    
    template<typename T, typename... Args>
    T* addComponent(Args&&... args) {
        auto component = allocator_->construct<T>(std::forward<Args>(args)...);
        components_.emplace_back(static_cast<Component*>(component), allocator_);
        return component;
    }
    
    template<typename T>
    T* getComponent() {
        for (auto& comp : components_) {
            T* typed = dynamic_cast<T*>(comp.get());
            if (typed) return typed;
        }
        return nullptr;
    }
    
    void update(float delta_time) {
        for (auto& component : components_) {
            component->update(delta_time);
        }
    }
    
    void print() const {
        std::cout << "  Entity: " << name_ << " (ID: " << id_ << ")\n";
        std::cout << "  Components (" << components_.size() << "):\n";
        for (const auto& component : components_) {
            component->print();
        }
    }
};

// Game Engine with hierarchical memory management
class GameEngine {
private:
    memento::scoped_init init_;
    memento::allocator root_allocator_;
    memento::allocator system_allocator_;
    memento::allocator graphics_allocator_;
    memento::allocator physics_allocator_;
    memento::allocator audio_allocator_;
    
    std::vector<memento::unique_ptr<Entity>> entities_;
    std::mt19937 random_gen_;
    
public:
    GameEngine() 
        : root_allocator_(memento::get_root_allocator())
        , system_allocator_(memento::allocator::create_proxy("system", &root_allocator_))
        , graphics_allocator_(memento::allocator::create_block("graphics", &system_allocator_))
        , physics_allocator_(memento::allocator::create_block("physics", &system_allocator_))
        , audio_allocator_(memento::allocator::create_thread_cache("audio"))
        , random_gen_(std::chrono::steady_clock::now().time_since_epoch().count()) {
        
        std::cout << "Game Engine initialized with hierarchical allocators:\n";
        std::cout << "  Root: " << root_allocator_.name() << "\n";
        std::cout << "  System: " << system_allocator_.name() << "\n";
        std::cout << "  Graphics: " << graphics_allocator_.name() << " (block allocator)\n";
        std::cout << "  Physics: " << physics_allocator_.name() << " (block allocator)\n";
        std::cout << "  Audio: " << audio_allocator_.name() << " (thread cache)\n\n";
    }
    
    ~GameEngine() {
        std::cout << "\nGame Engine shutting down...\n";
        printMemoryStats();
    }
    
    Entity* createEntity(const std::string& name) {
        int id = entities_.size() + 1;
        auto entity = system_allocator_.construct<Entity>(id, name, &system_allocator_);
        entities_.emplace_back(entity, &system_allocator_);
        return entity;
    }
    
    Entity* createPlayer(const std::string& name, float x, float y, float z) {
        auto player = createEntity(name);
        
        /* Add transform component using graphics allocator */
        /* auto transform = graphics_allocator_.construct<TransformComponent>(x, y, z); // Unused */
        player->addComponent<TransformComponent>(x, y, z);
        
        /* Add mesh component using graphics allocator */
        player->addComponent<MeshComponent>("player_mesh.obj", "player_material.mat");
        
        /* Add physics component using physics allocator */
        player->addComponent<PhysicsComponent>(75.0f, true);
        
        return player;
    }
    
    Entity* createEnemy(const std::string& name, float x, float y, float z) {
        auto enemy = createEntity(name);
        
        enemy->addComponent<TransformComponent>(x, y, z);
        enemy->addComponent<MeshComponent>("enemy_mesh.obj", "enemy_material.mat");
        enemy->addComponent<PhysicsComponent>(50.0f, true);
        
        return enemy;
    }
    
    Entity* createProp(const std::string& name, const std::string& mesh, const std::string& material) {
        auto prop = createEntity(name);
        
        float x = randomFloat(-50.0f, 50.0f);
        float y = 0.0f;
        float z = randomFloat(-50.0f, 50.0f);
        
        prop->addComponent<TransformComponent>(x, y, z);
        prop->addComponent<MeshComponent>(mesh, material);
        prop->addComponent<PhysicsComponent>(100.0f, false);  /* Static object */
        
        return prop;
    }
    
    void update(float delta_time) {
        /* Update all entities */
        for (auto& entity : entities_) {
            entity->update(delta_time);
        }
        
        /* Simulate game logic */
        if (!entities_.empty()) {
            /* Apply random forces to some entities */
            for (size_t i = 0; i < entities_.size() / 3; i++) {
                auto physics = entities_[i]->getComponent<PhysicsComponent>();
                if (physics) {
                    float fx = randomFloat(-10.0f, 10.0f);
                    float fy = randomFloat(0.0f, 20.0f);
                    float fz = randomFloat(-10.0f, 10.0f);
                    physics->applyForce(fx, fy, fz);
                }
            }
        }
    }
    
    void printEntities() const {
        std::cout << "\n=== Game Entities ===\n";
        std::cout << "Total entities: " << entities_.size() << "\n\n";
        
        for (const auto& entity : entities_) {
            entity->print();
            std::cout << "\n";
        }
    }
    
    void printMemoryStats() const {
        std::cout << "\n=== Memory Statistics ===\n";
        
        std::cout << "\nRoot Allocator:\n";
        root_allocator_.print_stats();
        
        std::cout << "\nSystem Allocator:\n";
        system_allocator_.print_stats();
        
        std::cout << "\nGraphics Allocator:\n";
        graphics_allocator_.print_stats();
        
        std::cout << "\nPhysics Allocator:\n";
        physics_allocator_.print_stats();
        
        std::cout << "\nAudio Allocator:\n";
        audio_allocator_.print_stats();
    }
    
private:
    float randomFloat(float min, float max) {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(random_gen_);
    }
};

int main(void) {
    std::cout << "Game Engine Memory Management Example\n";
    std::cout << "=====================================\n\n";
    
    try {
        /* Create game engine with RAII initialization */
        GameEngine engine;
        
        std::cout << "Creating game entities...\n\n";
        
        /* Create player */
        /* auto player = engine.createPlayer("Hero", 0.0f, 1.0f, 0.0f); // Unused for demo */
        
        /* Create enemies */
        /* auto enemy1 = engine.createEnemy("Goblin", 10.0f, 0.0f, 5.0f); // Unused for demo */
        /* auto enemy2 = engine.createEnemy("Orc", -15.0f, 0.0f, -8.0f); // Unused for demo */
        
        /* Create props (environment objects) */
        /* auto tree1 = engine.createProp("Tree1", "tree_model.obj", "bark_material.mat"); // Unused for demo */
        /* auto tree2 = engine.createProp("Tree2", "tree_model.obj", "bark_material.mat"); // Unused for demo */
        /* auto rock1 = engine.createProp("Rock1", "rock_model.obj", "stone_material.mat"); // Unused for demo */
        /* auto building = engine.createProp("House", "house_model.obj", "wood_material.mat"); // Unused for demo */
        
        std::cout << "\nInitial game state:\n";
        engine.printEntities();
        
        /* Simulate game updates */
        std::cout << "\nSimulating 5 game frames...\n";
        for (int frame = 1; frame <= 5; frame++) {
            std::cout << "\n--- Frame " << frame << " ---\n";
            engine.update(0.016f);  /* 60 FPS */
        }
        
        std::cout << "\nFinal game state:\n";
        engine.printEntities();
        
        /* Memory stats will be printed automatically when engine is destroyed */
        std::cout << "\nGame engine will be automatically cleaned up...\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\n✓ Game engine example completed successfully!" << std::endl;
    return 0;
}

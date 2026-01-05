/*
 * C++ Arena example
 * 
 * This example demonstrates how to use memento's arena feature for
 * efficient, type-safe allocation of objects with automatic cleanup.
 */

#include <iostream>
#include <string>
#include <vector>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

/* Game objects that will be allocated in arenas */
class GameObject {
protected:
    std::string name_;
    int id_;
    
public:
    GameObject(const std::string& name, int id) : name_(name), id_(id) {
        std::cout << "    GameObject created: " << name_ << " (ID: " << id_ << ")\n";
    }
    
    virtual ~GameObject() {
        std::cout << "    GameObject destroyed: " << name_ << " (ID: " << id_ << ")\n";
    }
    
    virtual void update(float delta_time) = 0;
    virtual void render() const = 0;
    
    const std::string& getName() const { return name_; }
    int getId() const { return id_; }
};

class Player : public GameObject {
private:
    float health_;
    float position_[3];
    
public:
    Player(int id, const std::string& name, float health, float x, float y, float z)
        : GameObject(name, id), health_(health) {
        position_[0] = x; position_[1] = y; position_[2] = z;
        std::cout << "      Player spawned at (" << x << ", " << y << ", " << z << ")\n";
    }
    
    void update(float delta_time) override {
        /* Simulate player movement */
        position_[0] += delta_time * 10.0f;
        health_ -= delta_time * 0.1f;
        if (health_ < 0) health_ = 0;
    }
    
    void render() const override {
        std::cout << "      Rendering player: " << name_ << " at (" 
                  << position_[0] << ", " << position_[1] << ", " << position_[2] 
                  << ") with health: " << health_ << "\n";
    }
};

class Enemy : public GameObject {
private:
    float damage_;
    bool active_;
    
public:
    Enemy(int id, const std::string& name, float damage)
        : GameObject(name, id), damage_(damage), active_(true) {
        std::cout << "      Enemy created with damage: " << damage_ << "\n";
    }
    
    void update(float /* delta_time */) override {
        /* Simple enemy AI */
        if (active_) {
            /* Move towards player (simplified) */
        }
    }
    
    void render() const override {
        std::cout << "      Rendering enemy: " << name_ << " (damage: " << damage_ << ")\n";
    }
    
    void deactivate() { active_ = false; }
    bool isActive() const { return active_; }
};

class Projectile : public GameObject {
private:
    float velocity_[3];
    float lifetime_;
    
public:
    Projectile(int id, const std::string& name, float vx, float vy, float vz)
        : GameObject(name, id), lifetime_(5.0f) {
        velocity_[0] = vx; velocity_[1] = vy; velocity_[2] = vz;
        std::cout << "      Projectile fired with velocity (" 
                  << vx << ", " << vy << ", " << vz << ")\n";
    }
    
    void update(float delta_time) override {
        /* Update position based on velocity */
        lifetime_ -= delta_time;
        if (lifetime_ <= 0) {
            std::cout << "      Projectile " << name_ << " expired\n";
        }
    }
    
    void render() const override {
        std::cout << "      Rendering projectile: " << name_ 
                  << " (lifetime: " << lifetime_ << ")\n";
    }
    
    bool isAlive() const { return lifetime_ > 0; }
};

/* Game system that uses arenas for object management */
class GameSystem {
private:
    memento::allocator allocator_;
    memento::arena<Player> player_arena_;
    memento::arena<Enemy> enemy_arena_;
    memento::arena<Projectile> projectile_arena_;
    
    std::vector<Player*> players_;
    std::vector<Enemy*> enemies_;
    std::vector<Projectile*> projectiles_;
    
    int next_id_;
    
public:
    GameSystem() 
        : allocator_(memento::allocator::create_thread_cache("game_system"))
        , player_arena_("player_arena", &allocator_)
        , enemy_arena_("enemy_arena", &allocator_)
        , projectile_arena_("projectile_arena", &allocator_)
        , next_id_(1) {
        std::cout << "  Game system initialized with arenas\n";
    }
    
    ~GameSystem() {
        std::cout << "  Game system shutting down\n";
        /* All objects in arenas will be automatically destroyed */
    }
    
    Player* spawnPlayer(const std::string& name, float health, float x, float y, float z) {
        std::cout << "  Spawning player: " << name << "\n";
        Player* player = player_arena_.make(next_id_, name, health, x, y, z);
        next_id_++;
        players_.push_back(player);
        return player;
    }
    
    Enemy* spawnEnemy(const std::string& name, float damage) {
        std::cout << "  Spawning enemy: " << name << "\n";
        Enemy* enemy = enemy_arena_.make(next_id_, name, damage);
        next_id_++;
        enemies_.push_back(enemy);
        return enemy;
    }
    
    Projectile* fireProjectile(const std::string& name, float vx, float vy, float vz) {
        std::cout << "  Firing projectile: " << name << "\n";
        Projectile* projectile = projectile_arena_.make(next_id_, name, vx, vy, vz);
        next_id_++;
        projectiles_.push_back(projectile);
        return projectile;
    }
    
    void update(float delta_time) {
        std::cout << "  Updating game objects (delta: " << delta_time << "s)\n";
        
        /* Update all game objects */
        for (Player* player : players_) {
            player->update(delta_time);
        }
        
        for (Enemy* enemy : enemies_) {
            if (enemy->isActive()) {
                enemy->update(delta_time);
            }
        }
        
        /* Remove dead projectiles */
        auto it = projectiles_.begin();
        while (it != projectiles_.end()) {
            (*it)->update(delta_time);
            if (!(*it)->isAlive()) {
                it = projectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    void render() const {
        std::cout << "  Rendering game objects:\n";
        
        for (const Player* player : players_) {
            player->render();
        }
        
        for (const Enemy* enemy : enemies_) {
            if (enemy->isActive()) {
                enemy->render();
            }
        }
        
        for (const Projectile* projectile : projectiles_) {
            if (projectile->isAlive()) {
                projectile->render();
            }
        }
    }
    
    void printStatistics() const {
        std::cout << "\n  Game System Statistics:\n";
        
        auto player_stats = player_arena_.stats();
        auto enemy_stats = enemy_arena_.stats();
        auto projectile_stats = projectile_arena_.stats();
        auto system_stats = allocator_.stats();
        
        std::cout << "    Players - Allocations: " << player_stats.allocation_count 
                  << ", Current usage: " << player_stats.current_usage << " bytes\n";
        std::cout << "    Enemies - Allocations: " << enemy_stats.allocation_count 
                  << ", Current usage: " << enemy_stats.current_usage << " bytes\n";
        std::cout << "    Projectiles - Allocations: " << projectile_stats.allocation_count 
                  << ", Current usage: " << projectile_stats.current_usage << " bytes\n";
        std::cout << "    System total - Allocations: " << system_stats.allocation_count 
                  << ", Current usage: " << system_stats.current_usage << " bytes\n";
    }
};

/* Demonstration of arena usage */
static void demonstrate_basic_arena(void) {
    std::cout << "\n2. Basic Arena Usage\n";
    std::cout << "   -----------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("basic_arena");
    memento::arena<int> int_arena("int_arena", &allocator);
    
    std::cout << "   Creating integer arena...\n";
    
    /* Allocate integers */
    int* num1 = int_arena.make(42);
    int* num2 = int_arena.make(100);
    int* num3 = int_arena.allocate(5);  /* Array of 5 integers */
    
    std::cout << "   Allocated values: *num1 = " << *num1 << ", *num2 = " << *num2 << "\n";
    std::cout << "   Array values: ";
    for (int i = 0; i < 5; i++) {
        num3[i] = i * 10;
        std::cout << num3[i] << " ";
    }
    std::cout << "\n";
    
    /* Show arena statistics */
    auto stats = int_arena.stats();
    std::cout << "   Arena statistics: " << stats.allocation_count << " allocations\n";
    
    /* Objects are automatically destroyed when arena goes out of scope */
    std::cout << "   Arena will be automatically cleaned up\n";
}

static void demonstrate_object_arena(void) {
    std::cout << "\n3. Object Arena Usage\n";
    std::cout << "   ------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("object_arena");
    
    std::cout << "   Creating separate arenas for different object types...\n";
    
    /* Create separate arenas for different object types */
    memento::arena<Player> player_arena("player_arena", &allocator);
    memento::arena<Enemy> enemy_arena("enemy_arena", &allocator);
    
    /* Create different types of game objects */
    Player* player = player_arena.make(1, std::string("Player1"), 100.0f, 0.0f, 0.0f, 0.0f);
    Enemy* enemy1 = enemy_arena.make(2, std::string("Enemy1"), 25.0f);
    Enemy* enemy2 = enemy_arena.make(3, std::string("Enemy2"), 50.0f);
    
    std::cout << "   Created " << player->getName() << ", " << enemy1->getName() << ", " << enemy2->getName() << "\n";
    
    /* Use the objects */
    player->update(0.1f);
    player->render();
    
    enemy1->update(0.1f);
    enemy1->render();
    
    /* Show statistics */
    auto player_stats = player_arena.stats();
    auto enemy_stats = enemy_arena.stats();
    std::cout << "   Player arena statistics: " << player_stats.allocation_count << " allocations\n";
    std::cout << "   Enemy arena statistics: " << enemy_stats.allocation_count << " allocations\n";
    
    std::cout << "   Objects will be automatically destroyed\n";
}

static void demonstrate_game_system(void) {
    std::cout << "\n4. Game System with Multiple Arenas\n";
    std::cout << "   ---------------------------------\n";
    
    GameSystem game;
    
    std::cout << "   Setting up game world...\n";
    
    /* Create game entities */
    Player* player = game.spawnPlayer("Hero", 100.0f, 0.0f, 0.0f, 0.0f);
    Enemy* goblin = game.spawnEnemy("Goblin", 25.0f);
    Enemy* orc = game.spawnEnemy("Orc", 50.0f);
    
    /* Verify entities were created (suppress unused variable warnings) */
    (void)player; (void)goblin; (void)orc;
    
    /* Fire some projectiles */
    Projectile* fireball = game.fireProjectile("Fireball", 10.0f, 0.0f, 0.0f);
    Projectile* arrow = game.fireProjectile("Arrow", 15.0f, 5.0f, 0.0f);
    
    /* Verify projectiles were created */
    (void)fireball; (void)arrow;
    
    std::cout << "\n   Initial game state:\n";
    game.printStatistics();
    
    /* Simulate game loop */
    std::cout << "\n   Simulating game loop...\n";
    for (int frame = 0; frame < 3; frame++) {
        std::cout << "\n   --- Frame " << frame + 1 << " ---\n";
        
        game.update(0.016f);  /* 60 FPS */
        game.render();
        
        if (frame == 1) {
            /* Fire another projectile */
            game.fireProjectile("Magic Missile", 20.0f, -5.0f, 0.0f);
        }
    }
    
    std::cout << "\n   Final game state:\n";
    game.printStatistics();
}

static void demonstrate_arena_performance(void) {
    std::cout << "\n5. Arena Performance Comparison\n";
    std::cout << "   -----------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("perf_test");
    
    /* Test regular allocation vs arena allocation */
    const int iterations = 10000;
    
    std::cout << "   Comparing performance (" << iterations << " allocations):\n";
    
    /* Regular allocation */
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<void*> regular_ptrs;
    regular_ptrs.reserve(iterations);
    
    for (int i = 0; i < iterations; i++) {
        void* ptr = allocator.allocate(64);
        regular_ptrs.push_back(ptr);
    }
    
    auto regular_time = std::chrono::high_resolution_clock::now() - start;
    
    /* Clean up regular allocations */
    for (void* ptr : regular_ptrs) {
        allocator.deallocate(ptr);
    }
    
    /* Arena allocation */
    memento::arena<int> int_arena("perf_arena", &allocator);
    
    start = std::chrono::high_resolution_clock::now();
    std::vector<int*> arena_ptrs;
    arena_ptrs.reserve(iterations);
    
    for (int i = 0; i < iterations; i++) {
        int* ptr = int_arena.allocate();
        arena_ptrs.push_back(ptr);
    }
    
    auto arena_time = std::chrono::high_resolution_clock::now() - start;
    
    auto regular_us = std::chrono::duration_cast<std::chrono::microseconds>(regular_time).count();
    auto arena_us = std::chrono::duration_cast<std::chrono::microseconds>(arena_time).count();
    
    std::cout << "   Regular allocation: " << regular_us << " μs\n";
    std::cout << "   Arena allocation: " << arena_us << " μs\n";
    std::cout << "   Arena is " << (regular_us > arena_us ? "faster" : "slower") 
              << " by " << (regular_us > arena_us ? regular_us - arena_us : arena_us - regular_us) 
              << " μs\n";
}

int main(void) {
    std::cout << "Memento C++ Arena Example\n";
    std::cout << "=========================\n\n";
    
    try {
        /* RAII initialization */
        memento::scoped_init init;
        
        std::cout << "1. Initializing with RAII...\n";
        
        /* Demonstrate different arena usage patterns */
        demonstrate_basic_arena();
        demonstrate_object_arena();
        demonstrate_game_system();
        demonstrate_arena_performance();
        
        std::cout << "\n✓ C++ arena example completed successfully!\n";
        std::cout << "\nKey takeaways:\n";
        std::cout << "- Arenas provide type-safe allocation\n";
        std::cout << "- Objects are automatically constructed and destroyed\n";
        std::cout << "- Multiple arenas can be used for different object types\n";
        std::cout << "- Arenas can be more efficient than individual allocations\n";
        std::cout << "- Memory is automatically cleaned up when arenas are destroyed\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

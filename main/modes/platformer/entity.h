#ifndef _ENTITY_H_
#define _ENTITY_H_

//==============================================================================
// Includes
//==============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "common_typedef.h"
#include "tilemap.h"
#include "gameData.h"

//==============================================================================
// Enums
//==============================================================================

typedef enum {
    ENTITY_PLAYER,
    ENTITY_TEST,
    ENTITY_SCROLL_LOCK_LEFT,
    ENTITY_SCROLL_LOCK_RIGHT,
    ENTITY_SCROLL_LOCK_UP,
    ENTITY_SCROLL_LOCK_DOWN,
    ENTITY_SCROLL_UNLOCK,
    ENTITY_HIT_BLOCK,
    ENTITY_DEAD,
    ENTITY_POWERUP,
    ENTITY_WARP,
    ENTITY_DUST_BUNNY,
    ENTITY_WASP
} entityIndex_t;

//==============================================================================
// Structs
//==============================================================================

typedef void(*updateFunction_t)(struct entity_t *self);
typedef void(*collisionHandler_t)(struct entity_t *self, struct entity_t *other);
typedef bool(*tileCollisionHandler_t)(struct entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);

struct entity_t
{
    bool active;
    //bool important;

    uint8_t type;
    updateFunction_t updateFunction;

    uint16_t x;
    uint16_t y;
    
    int16_t xspeed;
    int16_t yspeed;

    int16_t xMaxSpeed;
    int16_t yMaxSpeed;

    int16_t xDamping;
    int16_t yDamping;

    bool gravityEnabled;
    int16_t gravity;
    bool falling;

    uint8_t spriteIndex;
    bool spriteFlipHorizontal;
    bool spriteFlipVertical;
    uint8_t animationTimer;

    tilemap_t * tilemap;
    gameData_t * gameData;

    uint8_t homeTileX;
    uint8_t homeTileY;

    int16_t jumpPower;

    bool visible;
    uint8_t hp;
    int8_t invincibilityFrames;
    
    //entity_t *entities;
    entityManager_t *entityManager;

    collisionHandler_t collisionHandler;
    tileCollisionHandler_t tileCollisionHandler;
};

//==============================================================================
// Prototypes
//==============================================================================
void initializeEntity(entity_t * self, entityManager_t * entityManager, tilemap_t * tilemap, gameData_t * gameData);

void updatePlayer(entity_t * self);
void updateTestObject(entity_t * self);
void updateHitBlock(entity_t * self);

void moveEntityWithTileCollisions(entity_t * self);

void despawnWhenOffscreen(entity_t *self);

void destroyEntity(entity_t *self, bool respawn);

void applyDamping(entity_t *self);

void applyGravity(entity_t *self);

void animatePlayer(entity_t * self);

void detectEntityCollisions(entity_t *self);

void playerCollisionHandler(entity_t *self, entity_t* other);
void enemyCollisionHandler(entity_t *self, entity_t *other);
void dummyCollisionHandler(entity_t *self, entity_t *other);

bool playerTileCollisionHandler(entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);
bool enemyTileCollisionHandler(entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);
bool dummyTileCollisionHandler(entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);

void dieWhenFallingOffScreen(entity_t *self);

void updateDummy(entity_t* self);

void updateScrollLockLeft(entity_t* self);
void updateScrollLockRight(entity_t* self);
void updateScrollLockUp(entity_t* self);
void updateScrollLockDown(entity_t* self);
void updateScrollUnlock(entity_t* self);

void updateEntityDead(entity_t* self);

void updatePowerUp(entity_t* self);
void updateWarp(entity_t* self);

void updateDustBunny(entity_t* self);
bool dustBunnyTileCollisionHandler(entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);

void updateWasp(entity_t* self);
bool waspTileCollisionHandler(entity_t *self, uint8_t tileId, uint8_t tx, uint8_t ty, uint8_t direction);

void killEnemy(entity_t* target);

#endif

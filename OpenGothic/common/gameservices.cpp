#include "gameservices.h"

#include <cassert>

GameServices* GameServices::instance = nullptr;

GameServices::GameServices() {
  instance = this;
  }

GameServices::~GameServices() {
  instance = nullptr;
  }

GameServices& GameServices::inst() {
  assert(instance!=nullptr);
  return *instance;
  }

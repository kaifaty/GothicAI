#include "gamebasecontent.h"

GameBaseContent* GameBaseContent::instance = nullptr;

GameBaseContent::GameBaseContent() {
  instance = this;
  }

GameBaseContent::~GameBaseContent() {
  if(instance==this)
    instance = nullptr;
  }

GameBaseContent* GameBaseContent::inst() {
  return instance;
  }

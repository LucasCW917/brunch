#!/bin/bash
set -e

if ! pacman -Q sqlite &>/dev/null; then
  echo "Installing sqlite..."
  sudo pacman -S --noconfirm sqlite
fi

g++ -std=c++17 -O2 -o brunch_server main.cpp -lsqlite3 -lpthread
echo "Build complete: ./brunch_server"

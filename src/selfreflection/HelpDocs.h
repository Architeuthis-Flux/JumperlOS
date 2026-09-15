#ifndef HELPDOCS_H
#define HELPDOCS_H

#include <Arduino.h>

// Function to show help for a specific command
void showCommandHelp(char command);

// Open the interactive help browser. Returns the command char the user chose
// to run from its Commands screen, or 0 if they just left.
int showGeneralHelp();

// Function to show category-specific help
void showCategoryHelp(const char* category);

// Function to parse and handle help requests
bool handleHelpRequest(const char* input);

// Helper function to check if input is a help request
bool isHelpRequest(const char* input);

#endif

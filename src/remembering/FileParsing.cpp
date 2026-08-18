// SPDX-License-Identifier: MIT
#include "FileParsing.h"
#include "boards/board.h"
#include "PersistentStuff.h" // For firmwareVersion
#include "ArduinoJson.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "FilesystemStuff.h"  // For safe file operations
#include "FakeGpio.h"  // For fake GPIO state updates
// #include "LittleFS.h"
#include "Commands.h"
// #include "MachineCommands.h"
#include "MatrixState.h"
#include "States.h"
#include "NetManager.h"
#include "Probing.h"
#include "RotaryEncoder.h"
#include "SafeString.h"
// #include "menuTree.h"
#include "ArduinoStuff.h"
#include "CH446Q.h"
#include "Peripherals.h"
#include "config.h"
#include "Jerial.h" // Unified serial interface (includes TermControl and Jerial)
#include <Arduino.h>
#include <EEPROM.h>
#include <FatFS.h>

volatile bool netsUpdated = true;

// Source of truth is config.txt; readSettingsFromConfig() syncs debugFP at boot.
// (debugFPtime is a runtime-only timing flag toggled via the all-on/all-off menu.)
bool debugFP = false;
bool debugFPtime = false;

// LEGACY: These SafeString buffers are kept for backward compatibility with old slot files
// TODO: Eventually migrate all legacy file parsing to use States.cpp YAML format
// and remove these buffers entirely (would save 5.1KB)
// These three SafeStrings are static .bss (~6.1 KB). On the RP2040 (OG) that
// directly shrinks the heap, so size them down - OG slot files are smaller
// (fewer special-function nets, less routing). Each holds connection text; if
// ever truncated, the YAML state path in States.cpp is the real store.
#if defined(OG_JUMPERLESS)
createSafeString(nodeFileString, 900);
createSafeString(currentColorSlotColorsString, 512);
createSafeString(specialFunctionsString, 900);
#else
createSafeString(nodeFileString, 1800);
createSafeString(currentColorSlotColorsString, 1500);
createSafeString(specialFunctionsString, 2800);
#endif

// General-purpose nodeFileString backup/restore system
// // static char nodeFileStringBackup[1800];
// static bool nodeFileBackupStored = false;

// Track which slots have net colors assigned (bit mask for performance)
uint32_t slotsWithNetColors = 0;

// Track which slots have been validated (bit mask for performance)
uint32_t slotsValidated = 0;

int numConnsJson = 0;

// char inputBuffer[INPUTBUFFERLENGTH] = {0};

// ArduinoJson::StaticJsonDocument<8000> wokwiJson;
// ;

// String connectionsW[MAX_BRIDGES][5];

File nodeFile;

File wokwiFile;

File nodeFileBuffer;

File colorFile; // Added for net color storage

unsigned long timeToFP = 0;

enum openType {
  w = 0,
  wplus = 1,
  r = 2,
  rplus = 3,
  a = 4,
  aplus = 5,

};

const char rotaryConnectionString[] =
    "{AREF-GND,D11-GND,D10-UART_TX,D12-UART_RX,D13-GPIO_0, ";

void closeAllFiles() {
  fs_mutex_acquire();  // THREAD SAFETY: Lock filesystem
  
  if (nodeFile) {
    nodeFile.flush();
    nodeFile.close();
  }
  // if (wokwiFile) {
  //   wokwiFile.close();
  // }
  if (nodeFileBuffer) {
    nodeFileBuffer.flush();
    nodeFileBuffer.close();
  }
  
  fs_mutex_release();  // THREAD SAFETY: Unlock filesystem
}

// CONTRACT: this only opens the global nodeFile - it does NOT protect the
// subsequent read/write/close. The sole live caller (openNodeFile) wraps the
// whole open->read->close in its own fs_mutex_acquire/release; fs_mutex is
// per-core recursive, so the acquire/release here just nests inside that.
// core1busy is owned by the caller for the same reason - it has to stay set
// across the read, not just the open.
int openFileThreadSafe(int openTypeEnum, int slot, int flashOrLocal) {

  // THREAD SAFETY: Use proper mutex instead of busy-wait
  fs_mutex_acquire();

  {
    unsigned long _t = micros();
    while (core2busy) {
      __dmb();
      // ponytail: timeout-and-proceed only; core2busy belongs to core 2,
      // force-clearing it here lied to every other waiter
      if (micros() - _t > 25000) break;
      TinyUSB_Device_Task(); // mutex-guarded pump
    }
  }

  // Jerial.println(micros() - start);
  if (nodeFile) {
    // Jerial.println("nodeFile is open");
    nodeFile.close();
  }

  switch (openTypeEnum) {
  case 0:
    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "w");
    break;
  case 1:
    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "w+");
    break;
  case 2:
    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "r");
    break;
  case 3:

    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "r+");
    break;
  case 4:
    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "a");
    break;
  case 5:

    nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "a+");
    break;
  default:
    break;
  }

  if (!nodeFile) {
    // if (debugFP)
    //  Jerial.println("\n\n\rFailed to open nodeFile\n\n\r");
    // openFileThreadSafe(w, slot);
    fs_mutex_release();  // THREAD SAFETY: Unlock on early return
    return 0;
  } else {
    if (debugFP)
      Jerial.println(
          "\n\ropened nodeFile.txt\n\n\rloading bridges from file\n\r");
  }
  // Jerial.print("openFileThreadSafe done   ");
  // Jerial.println(micros() - start);
  fs_mutex_release();  // THREAD SAFETY: Unlock filesystem
  return 1;
}

// ponytail: gutted to a no-op. The old body opened /MenuTree.txt with "w"
// while every content write was commented out, so a call truncated the file
// to zero bytes. The symbol has to survive because Menus.cpp readMenuFile(0)
// still links against it (out of this fix's edit scope); that branch is dead
// at runtime - readMenuFile is only ever called with 1. Delete this stub,
// the FileParsing.h declaration, and the Menus.cpp call together.
void writeMenuTree(void) {}

// void createLocalNodeFile(int slot) {
//   // MIGRATED: Now loads slot into globalState via SlotManager
//   loadSlotIntoState(slot);
// }

void loadSlotIntoState(int slot) {
  // NEW: Modern replacement for openNodeFile() / createLocalNodeFile()
  // Loads a slot file directly into globalState using SlotManager
  SlotManager& mgr = SlotManager::getInstance();
  String errorMsg;
  
  if (!mgr.loadSlot(slot, errorMsg)) {
    if (debugFP) {
      Jerial.println("Error loading slot " + String(slot) + ": " + errorMsg);
    }
    // Clear state on error
    globalState.clearAllConnections();
  } else {
    if (debugFP) {
      Jerial.println("✓ Loaded slot " + String(slot) + " into globalState (" + 
                    String(globalState.connections.numBridges) + " connections)");
    }
  }
}

void clearNodeFileString() { nodeFileString.clear(); }

//==============================================================================
// New RAM-based state functions - Preferred over legacy file-based functions
//==============================================================================

/**
 * @brief Add a bridge connection to globalState (RAM only, no file I/O)
 * @param node1 First node number
 * @param node2 Second node number  
 * @param duplicates Number of parallel paths (-1 = use default from config)
 * @param autoRefresh If true, immediately refresh hardware (default: true for single ops, set false for batch)
 * @return true if added successfully, false if invalid
 */
bool addBridgeToState(int node1, int node2, int duplicates, bool autoRefresh) {
    String errorMsg;
    
    // Use the state's built-in validation and addition
    bool success = globalState.addConnection(node1, node2, errorMsg, duplicates);
    
    if (!success) {
        if (debugFP) {
            Jerial.print("addBridgeToState failed: ");
            Jerial.println(errorMsg);
        }
        return false;
    }
    
    // Mark as dirty for future save
    globalState.markDirty();
    
    // Optionally update hardware immediately
    if (autoRefresh) {
        refreshLocalConnections(1, 1, 0);
    }
    
    // Update fake GPIO pins if this bridge affects their routing
    updateFakeGpioAfterConnectionChange(node1, node2);
    
    if (debugFP) {
        Jerial.print("Added bridge to state: ");
        Jerial.print(node1);
        Jerial.print(" - ");
        Jerial.println(node2);
    }
    
    return true;
}

/**
 * @brief Remove a bridge connection from globalState (RAM only, no file I/O)
 * @param node1 First node number
 * @param node2 Second node number (or -1 to remove ALL connections containing node1)
 * @param autoRefresh If true, immediately refresh hardware (default: true for single ops, set false for batch)
 * @return true if removed successfully, false if not found or invalid
 */
bool removeBridgeFromState(int node1, int node2, bool autoRefresh) {
    String errorMsg;
    bool success = false;
    int removedCount = 0;
    
    // Clear the lastRemovedNodes tracking array
    lastRemovedNodesIndex = 0;
    for (int i = 0; i < 20; i++) {
        lastRemovedNodes[i] = -1;
    }
    
    if (node2 == -1) {
        // Remove ALL connections containing node1
        // We need to iterate through bridges and remove any that contain node1
        int numBridges = globalState.connections.numBridges;
        
        // Iterate backwards so we can remove without index issues
        for (int i = numBridges - 1; i >= 0; i--) {
            int bridgeNode1 = globalState.connections.bridges[i][0];
            int bridgeNode2 = globalState.connections.bridges[i][1];
            
            if (bridgeNode1 == node1 || bridgeNode2 == node1) {
                // Track the OTHER node that was disconnected
                int otherNode = (bridgeNode1 == node1) ? bridgeNode2 : bridgeNode1;
                if (lastRemovedNodesIndex < 20) {
                    lastRemovedNodes[lastRemovedNodesIndex++] = otherNode;
                }
                
                // Remove this bridge
                if (globalState.removeConnection(bridgeNode1, bridgeNode2, errorMsg)) {
                    removedCount++;
                    success = true;
                }
            }
        }
        
        if (removedCount > 0) {
            disconnectedNodeNewData = true;
        }
        
    } else {
        // Remove specific connection node1-node2
        success = globalState.removeConnection(node1, node2, errorMsg);
        
        if (success) {
            removedCount = 1;
            // Track both nodes as removed
            lastRemovedNodes[0] = node1;
            lastRemovedNodes[1] = node2;
            lastRemovedNodesIndex = 2;
            disconnectedNodeNewData = true;
        }
    }
    
    if (!success) {
        if (debugFP) {
            Jerial.print("removeBridgeFromState failed: ");
            Jerial.println(errorMsg);
        }
        return false;
    }
    
    // Mark as dirty for future save
    globalState.markDirty();
    
    // Optionally update hardware immediately
    if (autoRefresh && removedCount > 0) {
        refreshLocalConnections(-1, 1, 0);
    
    
    // Update fake GPIO pins if this bridge affects their routing
    updateFakeGpioAfterConnectionChange(node1, node2);
    }
    
    if (debugFP) {
        if (node2 == -1) {
            Jerial.print("Removed ");
            Jerial.print(removedCount);
            Jerial.print(" bridges containing node ");
            Jerial.println(node1);
        } else {
            Jerial.print("Removed bridge from state: ");
            Jerial.print(node1);
            Jerial.print(" - ");
            Jerial.println(node2);
        }
    }
    
    return true;
}

/**
 * @brief Save globalState to YAML file
 * @param slot Slot number to save to (-1 = use current netSlot)
 * @return true if saved successfully
 */
bool saveStateToSlot(int slot) {
    if (slot == -1) {
        slot = netSlot;
    }
    
    String errorMsg;
    SlotManager& mgr = SlotManager::getInstance();
    
    bool success = mgr.saveSlot(slot, errorMsg);
    
    if (!success) {
        Jerial.print("Failed to save state to slot ");
        Jerial.print(slot);
        Jerial.print(": ");
        Jerial.println(errorMsg);
        return false;
    }
    
    if (debugFP) {
        Jerial.print("Saved state to slot ");
        Jerial.println(slot);
    }
    
    return true;
}



void createSlots(int slot, int overwrite) {
  // Create slots directory if it doesn't exist
  if (!safeMkdir("/slots")) {
    if (debugFP) {
      Jerial.println("Failed to create /slots/ directory");
    }
    return;
  }

  // Create python_scripts directory if it doesn't exist
  if (!safeMkdir("/python_scripts")) {
    if (debugFP) {
      Jerial.println("Failed to create /python_scripts/ directory");
    }
    return;
  }

  // Create empty history.txt file if it doesn't exist
  if (!safeFileExists("/python_scripts/history.txt", 500)) {
    if (safeFileWriteAll("/python_scripts/history.txt", "", 0, 1000)) {
      if (debugFP) {
        Jerial.println("Created empty /history.txt file");
      }
    } else {
      if (debugFP) {
        Jerial.println("Failed to create /history.txt file");
      }
    }
  }

  // Use SlotManager to create YAML slot files
  SlotManager& mgr = SlotManager::getInstance();
  
  if (slot == -1) {
    // Create all slots
    for (int i = 0; i < NUM_SLOTS; i++) {
      String yamlPath = "/slots/slot" + String(i) + ".yaml";
      
      if (overwrite == 1) {
        // Overwrite: always create/clear the slot
        if (debugFP) {
          Jerial.println("Creating/clearing slot " + String(i));
        }
        mgr.ensureSlotExists(i);
      } else {
        // Don't overwrite: only create if doesn't exist
        if (!FatFS.exists(yamlPath.c_str())) {
          if (debugFP) {
            Jerial.println("Creating new slot " + String(i));
          }
          mgr.ensureSlotExists(i);
        } else {
          if (debugFP) {
            Jerial.println("Slot " + String(i) + " already exists, skipping");
          }
        }
      }
    }
  } else {
    // Create single slot
    if (debugFP) {
      Jerial.println("Creating slot " + String(slot));
    }
    mgr.ensureSlotExists(slot);
  }
  
  refreshPaths();
}

void createConfigFile(int overwrite) {

  if (overwrite == 0) {
    if (safeFileExists("config.txt")) {
      return;
    }
  }

  File configFile = safeFileOpen("config.txt", "w");
  if (configFile) {
    const char* header = "#Jumperless Config file\n\r";
    configFile.write((const uint8_t*)header, strlen(header));
    const char* verLabel = "version: 5\n\r";
    configFile.write((const uint8_t*)verLabel, strlen(verLabel));
    const char* revLabel = "revision: ";
    configFile.write((const uint8_t*)revLabel, strlen(revLabel));
    String r = String(EEPROM.read(REVISIONADDRESS)) + "\n\r";
    configFile.write((const uint8_t*)r.c_str(), r.length());

    safeFileClose(configFile, true);
  }
}

// int checkIfBridgeExists(int node1, int node2, int slot, int flashOrLocal) {

//   return removeBridgeFromNodeFile(node1, node2, slot, flashOrLocal, 1);
//   if (flashOrLocal == 0) {

//     openFileThreadSafe(rplus, slot);

//     if (!nodeFile) {
//       if (debugFP) {
//         // Jerial.println("Failed to open nodeFile (removeBridgeFromNodeFile)");
//       }

//       return -1;
//     } else {
//       if (debugFP)
//         Jerial.println(
//             "\n\ropened nodeFile.txt\n\n\rloading bridges from file\n\r");
//     }

//     if (nodeFile.size() < 2) {
//       // Jerial.println("empty file");
//       nodeFile.close();
//       core1busy = false;
//       return -1;
//     }
//     nodeFile.seek(0);
//     nodeFile.setTimeout(8);

//     // nodeFile.readStringUntil
//   }

//   return 0;
// }

void inputNodeFileList(int addRotaryConnections) {
  // NEW: Parse node file lists directly into YAML slots using the state system
  // Format: "o Slot 0 f { 8-17, 21-28, ... }"
  
  // Jerial.println("Reading node file list from Jerial...");

  unsigned long humanTime = millis();

  int shown = 0;
  while (Jerial.available() == 0) {
    if (millis() - humanTime == 400 && shown == 0) {
      Jerial.println("Paste the nodeFile list here\n\n\r");
      shown = 1;
    }
  }
  nodeFileString.clear();

  int startInsertion = 0;

  nodeFileString.read(Jerial);
  if (debugFP) {
    Jerial.println("Raw input:");
    nodeFileString.printTo(Jerial);
    Jerial.println();
  }

  nodeFileString.trim();
  
  // NEW: Parse and save slots directly to YAML without making them active
  // Format: "Slot 0 f { 8-17, 21-28, ... }" OR just "f { 8-17, 21-28, ... }"
  // If no "Slot X" prefix, use the currently active slot
  String input = nodeFileString.c_str();
  int startIdx = 0;
  int slotsProcessed = 0;
  
  // Check if input starts with "Slot" keyword
  bool hasSlotPrefix = (input.indexOf("Slot ") == 0 || input.indexOf("slot ") == 0 ||
                        input.indexOf("Slot ") >= 0 || input.indexOf("slot ") >= 0);
  
  if (!hasSlotPrefix) {
    // No "Slot X" prefix - use active slot and parse directly
    if (debugFP) {
      Jerial.println("◆ No slot prefix found, using active slot " + String(netSlot));
    }
    
    // Find connection data between { and }
    int openBrace = input.indexOf('{');
    int closeBrace = input.indexOf('}', openBrace + 1);
    
    if (openBrace == -1 || closeBrace == -1) {
      Jerial.println("◇ Missing braces in input");
      Jerial.println("◇ Expected format: f { node-node, node-node, ... }");
      Jerial.println("◇ Or: Slot [number] f { node-node, node-node, ... }");
      return;
    }
    
    // Extract connection string
    String connections = input.substring(openBrace + 1, closeBrace);
    connections.trim();
    
    if (debugFP) {
      Jerial.println("◆ Active slot " + String(netSlot) + " connections: " + connections);
    }
    
    // NEVER copy JumperlessState - work directly with the singleton
    SlotManager& mgr = SlotManager::getInstance();
    String errorMsg;
    
    // Use the active slot
    int slotNum = netSlot;
    
    // Clear the active state and populate it directly (NO COPIES!)
    mgr.getActiveState().clear();
    
    // Get direct reference to the active state (no copying!)
    JumperlessState& state = mgr.getActiveState();
    
    // Parse connections directly into the active state
    int connCount = 0;
    int connIdx = 0;
    
    while (connIdx < connections.length()) {
      int commaIdx = connections.indexOf(',', connIdx);
      if (commaIdx == -1) {
        commaIdx = connections.length();
      }
      
      String conn = connections.substring(connIdx, commaIdx);
      conn.trim();
      
      if (conn.length() > 0) {
        int dashIdx = conn.indexOf('-');
        if (dashIdx > 0 && dashIdx < conn.length() - 1) {
          int node1 = conn.substring(0, dashIdx).toInt();
          int node2 = conn.substring(dashIdx + 1).toInt();
          
          if (node1 > 0 && node2 > 0) {
            if (state.connections.numBridges < MAX_BRIDGES) {
              state.connections.bridges[state.connections.numBridges][0] = node1;
              state.connections.bridges[state.connections.numBridges][1] = node2;
              state.connections.bridges[state.connections.numBridges][2] = -1;
              state.connections.numBridges++;
              connCount++;
              
              if (debugFP) {
                Jerial.println("  Added: " + String(node1) + "-" + String(node2));
              }
            } else {
              Jerial.println("◇ Warning: Max bridges reached");
              break;
            }
          }
        }
      }
      
      connIdx = commaIdx + 1;
    }

    // Use centralized locked connections handler instead of inline logic
    // (locked connections are re-added by infraEvaluate at the next rebuild)
    
   // Jerial.println("◆ Active slot " + String(slotNum) + ": parsed " + String(connCount) + " connections");
    
    // Save and apply to active slot
    if (mgr.saveSlot(slotNum, errorMsg)) {
     // Jerial.println("  ✓ Saved to /slots/slot" + String(slotNum) + ".yaml");
     // Jerial.println("  ↻ Reloading active slot to apply changes...");
      if (mgr.loadSlot(slotNum, errorMsg)) {
      //  Jerial.println("  ✓ Applied to hardware");
      } else {
        Jerial.println("  ✗ Failed to apply: " + errorMsg);
      }
    } else {
      Jerial.println("  ✗ Failed to save: " + errorMsg);
    }
    
    return;
  }
  
  // Has "Slot X" prefix - parse multiple slots
  while (startIdx >= 0 && startIdx < input.length()) {
    // Find "Slot " keyword
    int slotIdx = input.indexOf("Slot ", startIdx);
    if (slotIdx == -1) {
      slotIdx = input.indexOf("slot ", startIdx); // Try lowercase
    }
    if (slotIdx == -1) {
      break; // No more slots found
    }
    
    // Extract slot number
    int slotNum = -1;
    int digitIdx = slotIdx + 5; // Position after "Slot "
    while (digitIdx < input.length() && (input[digitIdx] == ' ' || input[digitIdx] == '\t')) {
      digitIdx++; // Skip spaces
    }
    if (digitIdx < input.length() && isDigit(input[digitIdx])) {
      slotNum = input[digitIdx] - '0';
      // Handle multi-digit slot numbers
      digitIdx++;
      while (digitIdx < input.length() && isDigit(input[digitIdx])) {
        slotNum = slotNum * 10 + (input[digitIdx] - '0');
        digitIdx++;
      }
    }
    
    if (slotNum < 0 || slotNum >= NUM_SLOTS) {
      Jerial.println("◇ Invalid slot number: " + String(slotNum));
      startIdx = digitIdx;
      continue;
    }
    
    // Find connection data between { and }
    int openBrace = input.indexOf('{', digitIdx);
    int closeBrace = input.indexOf('}', openBrace + 1);
    
    if (openBrace == -1 || closeBrace == -1) {
      Jerial.println("◇ Missing braces for slot " + String(slotNum));
      startIdx = digitIdx;
      continue;
    }
    
    // Extract connection string
    String connections = input.substring(openBrace + 1, closeBrace);
    connections.trim();
    
    if (debugFP) {
      Jerial.println("◆ Slot " + String(slotNum) + " connections: " + connections);
    }
    
    // NEVER copy JumperlessState - work directly with the singleton
    SlotManager& mgr = SlotManager::getInstance();
    String errorMsg;
    
    // Remember which slot was active
    int savedActiveSlot = mgr.getActiveSlot();
    bool needToRestore = (savedActiveSlot != slotNum);
    
    // Clear the active state and populate it directly (NO COPIES!)
    mgr.getActiveState().clear();
    mgr.setActiveSlot(slotNum);
    
    // Get direct reference to the active state (no copying!)
    JumperlessState& state = mgr.getActiveState();
    
    // Parse connections directly into the active state
    // Format: "8-17, 21-28, 52-43, ..."
    int connCount = 0;
    int connIdx = 0;
    
    while (connIdx < connections.length()) {
      // Find next comma or end
      int commaIdx = connections.indexOf(',', connIdx);
      if (commaIdx == -1) {
        commaIdx = connections.length();
      }
      
      String conn = connections.substring(connIdx, commaIdx);
      conn.trim();
      
      if (conn.length() > 0) {
        // Parse "node1-node2"
        int dashIdx = conn.indexOf('-');
        if (dashIdx > 0 && dashIdx < conn.length() - 1) {
          int node1 = conn.substring(0, dashIdx).toInt();
          int node2 = conn.substring(dashIdx + 1).toInt();
          
          if (node1 > 0 && node2 > 0) {
            // Add bridge directly to active state (NO TEMP OBJECTS!)
            // bridges is int[MAX_BRIDGES][3] where [i][0]=node1, [i][1]=node2, [i][2]=duplicates
            if (state.connections.numBridges < MAX_BRIDGES) {
              state.connections.bridges[state.connections.numBridges][0] = node1;
              state.connections.bridges[state.connections.numBridges][1] = node2;
              state.connections.bridges[state.connections.numBridges][2] = -1; // duplicates
              state.connections.numBridges++;
              connCount++;
              
              if (debugFP) {
                Jerial.println("  Added: " + String(node1) + "-" + String(node2));
              }
            } else {
              Jerial.println("◇ Warning: Max bridges reached for slot " + String(slotNum));
              break;
            }
          }
        }
      }
      
      connIdx = commaIdx + 1;
    }
    
    // Use centralized locked connections handler
    // (locked connections are re-added by infraEvaluate at the next rebuild)
    
    Jerial.println("◆ Slot " + String(slotNum) + ": parsed " + String(connCount) + " connections");
    
    // Save the populated state to the slot
    if (mgr.saveSlot(slotNum, errorMsg)) {
      Jerial.println("  ✓ Saved to /slots/slot" + String(slotNum) + ".yaml");
      slotsProcessed++;
      
      // If we modified a non-active slot, reload the original active slot
      if (needToRestore) {
        // Reload the original active slot to restore hardware state
        mgr.loadSlot(savedActiveSlot, errorMsg);
      } else {
        // We just saved the active slot, reload it to apply changes
        Jerial.println("  ↻ Reloading active slot to apply changes...");
        if (mgr.loadSlot(slotNum, errorMsg)) {
          Jerial.println("  ✓ Applied to hardware");
        } else {
          Jerial.println("  ✗ Failed to apply: " + errorMsg);
        }
      }
    } else {
      Jerial.println("  ✗ Failed to save: " + errorMsg);
      // Restore the original slot on failure
      if (needToRestore) {
        mgr.loadSlot(savedActiveSlot, errorMsg);
      }
    }
    
    // Move to next slot
    startIdx = closeBrace + 1;
  }
  
  if (slotsProcessed == 0) {
    Jerial.println("◇ No valid slots found in input");
    Jerial.println("◇ Expected format: Slot [number] f { node-node, node-node, ... }");
    return;
  }
  
  Jerial.println("◆ Successfully processed " + String(slotsProcessed) + " slot(s)");
  
  // OLD CODE REMOVED - migrated to YAML-based state system above
}

void saveCurrentSlotToSlot(int slotFrom, int slotTo, int flashOrLocalfrom,
                           int flashOrLocalTo) {
  // MIGRATED: Now uses SlotManager with YAML state system
  SlotManager& mgr = SlotManager::getInstance();
  String errorMsg;
  
  // Load source slot into globalState
  if (!mgr.loadSlot(slotFrom, errorMsg)) {
    Jerial.println("Error loading slot " + String(slotFrom) + ": " + errorMsg);
    return;
  }
  
  // Save globalState to destination slot
  if (!mgr.saveSlot(slotTo, errorMsg)) {
    Jerial.println("Error saving to slot " + String(slotTo) + ": " + errorMsg);
    return;
  }
  
  // Update active slot tracking
  netSlot = slotTo;
  mgr.setActiveSlot(slotTo);
}

void savePreformattedNodeFile(int source, int slot, int keepEncoder, const String& preformattedData) {
  // NEW: Parse input directly into RAM state (no file I/O until auto-save)
  unsigned long startTime = micros();
  specialFunctionsString.clear();
  
  if (source == 0 || true) {
    // Handle serial command buffer (for UART connections)
    if (serialCommandBufferIndex > 0) {
      specialFunctionsString.print("{ 116-70, 117-71, ");
      serialCommandBufferIndex = 0;
    }

    // Check if line buffering is enabled or if we have preformatted data (e.g., from relayed commands)
    extern struct config jumperlessConfig;
    // CRITICAL FIX: Always use preformattedData if it has content > 1 char (includes relayed commands!)
    // Line buffering check is secondary - relayed commands should ALWAYS use preformatted path
    bool usePreformattedData = (preformattedData.length() > 1);
    
    if (usePreformattedData) {
      // Line buffering mode OR relayed command: use the pre-parsed complete line
      // Skip the first character (command) and any leading whitespace
      String dataOnly = preformattedData;
      if (dataOnly.length() > 0) {
        dataOnly = dataOnly.substring(1); // Skip 'f' command character
      }
      dataOnly.trim();
      if (debugFP) {
        Jerial.print("DEBUG: Using preformatted data = '");
        Jerial.print(dataOnly);
        Jerial.println("'");
      }
      specialFunctionsString = dataOnly.c_str();
    } else {
      // Single-char mode: read directly from serial (old behavior)
      if (debugFP) {
        Jerial.println("DEBUG: Reading from Serial (no preformatted data)");
      }
      specialFunctionsString.read(Jerial);
    }
    specialFunctionsString.trim();
    if (debugFP) {
      Jerial.print("DEBUG: specialFunctionsString after trim = '");
      Jerial.print(specialFunctionsString);
      Jerial.println("'");
    }
    if (specialFunctionsString.endsWith(",") == 0) {
      specialFunctionsString.concat(",\n\r");
    }
    
    // Replace special names with node numbers
    replaceSFNamesWithDefinedInts();
    replaceNanoNamesWithDefinedInts();

    
    // Parse into RAM state instead of file
    // Clear current connections in state
    if (debugFP) {
      Jerial.println("DEBUG: Clearing all existing connections...");
    }
    globalState.clearAllConnections();
    if (debugFP) {
      Jerial.println("DEBUG: All connections cleared. Starting fresh parse...");
    }
    
    // Parse the connections from specialFunctionsString
    // Format: "1-2, 3-4, 5-6, ..."
    specialFunctionsString.replace("{", "");
    specialFunctionsString.replace("}", "");
    specialFunctionsString.replace("\n", "");
    specialFunctionsString.replace("\r", "");
    specialFunctionsString.trim();
    
    // Parse each connection using SafeString tokenizer
    int connCount = 0;
    createSafeString(connBuf, 20);
    createSafeString(nodeBuf, 10);
    char commaDelim[] = ",";
    char dashDelim[] = "-";
    createSafeStringFromCharArray(commaDelimiters, commaDelim);
    createSafeStringFromCharArray(dashDelimiters, dashDelim);
    
    if (debugFP) {
      Jerial.print("DEBUG: About to parse: '");
      Jerial.print(specialFunctionsString);
      Jerial.print("' (length=");
      Jerial.print(specialFunctionsString.length());
      Jerial.println(")");
    }
    
    int stringIndex = 0;
    while (stringIndex >= 0) {
      stringIndex = specialFunctionsString.stoken(connBuf, stringIndex, commaDelimiters);
      if (debugFP) {
        Jerial.print("DEBUG: stoken returned stringIndex=");
        Jerial.print(stringIndex);
        Jerial.print(", connBuf='");
        Jerial.print(connBuf);
        Jerial.println("'");
      }
      if (stringIndex == -1) break;
      
      connBuf.trim();
      if (debugFP) {
        Jerial.print("DEBUG: After trim, connBuf='");
        Jerial.print(connBuf);
        Jerial.print("' (length=");
        Jerial.print(connBuf.length());
        Jerial.println(")");
      }
      
      if (connBuf.length() < 3) {
        if (debugFP) {
          Jerial.println("DEBUG: Skipping - too short");
        }
        continue;  // Skip empty tokens
      }
      
      // Parse "node1-node2" format using SafeString
      int nodeIndex = 0;
      nodeIndex = connBuf.stoken(nodeBuf, nodeIndex, dashDelimiters);
      if (debugFP) {
        Jerial.print("DEBUG: First stoken: nodeIndex=");
        Jerial.print(nodeIndex);
        Jerial.print(", nodeBuf='");
        Jerial.print(nodeBuf);
        Jerial.println("'");
      }
      if (nodeIndex == -1) {
        if (debugFP) {
          Jerial.println("DEBUG: First token failed");
        }
        continue;
      }
      
      int node1 = 0;
      nodeBuf.toInt(node1);
      if (debugFP) {
        Jerial.print("DEBUG: node1=");
        Jerial.println(node1);
      }
      
      // stoken returns the position OF the delimiter, not after it
      // We need to skip past the delimiter (which is 1 character: '-')
      if (nodeIndex < connBuf.length()) {
        nodeIndex++; // Skip past the '-' delimiter
        if (debugFP) {
          Jerial.print("DEBUG: Adjusted nodeIndex to ");
          Jerial.println(nodeIndex);
        }
      }
      
      nodeIndex = connBuf.stoken(nodeBuf, nodeIndex, dashDelimiters);
      if (debugFP) {
        Jerial.print("DEBUG: Second stoken: nodeIndex=");
        Jerial.print(nodeIndex);
        Jerial.print(", nodeBuf='");
        Jerial.print(nodeBuf);
        Jerial.println("'");
      }
      if (nodeIndex == -1 && nodeBuf.length() == 0) {
        // Only fail if we didn't get any data - if nodeIndex is -1 but nodeBuf has data,
        // it just means this was the last token
        if (debugFP) {
          Jerial.println("DEBUG: Second token failed - no data");
        }
        continue;
      }
      
      int node2 = 0;
      nodeBuf.toInt(node2);
      if (debugFP) {
        Jerial.print("DEBUG: node2=");
        Jerial.println(node2);
      }
      
      if (debugFP) {
        Jerial.print("DEBUG: Checking if node1 > 0 && node2 > 0: ");
        Jerial.print(node1);
        Jerial.print(" > 0 && ");
        Jerial.print(node2);
        Jerial.println(" > 0");
      }
      
      if (node1 > 0 && node2 > 0) {
        if (debugFP) {
          Jerial.println("DEBUG: Calling addBridgeToState()");
        }
        // Add to RAM state (no auto-refresh for batch operation)
        if (addBridgeToState(node1, node2, -1, false)) {
          connCount++;
          if (debugFP) {
            Jerial.print("DEBUG: Successfully added connection, connCount=");
            Jerial.println(connCount);
            Jerial.print("Parsed: ");
            Jerial.print(node1);
            Jerial.print("-");
            Jerial.println(node2);
          }
        } else {
          if (debugFP) {
            Jerial.println("DEBUG: addBridgeToState() returned false");
          }
        }
      } else {
        if (debugFP) {
          Jerial.println("DEBUG: Skipping - node1 or node2 is zero or negative");
        }
      }
    }
    
    // Jerial.print("Loaded ");
    // Jerial.print(connCount);
    // Jerial.println(" connections into RAM (will auto-save)");
    // unsigned long endTime = micros();
    // unsigned long duration = endTime - startTime;
    // Jerial.print("Parse time: ");
    // Jerial.print(duration);
    // Jerial.println(" us");
    // Jerial.flush();
    
    // Mark as dirty and refresh immediately (user needs instant hardware feedback)
    if (connCount > 0) {
      globalState.markDirty();
      
      // Refresh immediately but with optimizations to keep it fast
      unsigned long refreshStart = millis();
      refreshLocalConnections(-1, 1, 1);
      unsigned long refreshTime = millis() - refreshStart;
      
      // Warn if refresh is taking too long (indicates performance problem)
      if (refreshTime > 300) {
        Serial.print("⚠️  Slow refresh: ");
        Serial.print(refreshTime);
        Serial.println(" ms");
        Serial.flush();
      }
    }

    // endTime = micros();
    // duration = endTime - startTime;
    // Jerial.print("After Refresh: ");
    // Jerial.print(duration);
    // Jerial.println(" us");
    // Jerial.flush();
  }
  
  // No file write here - auto-save scheduler handles it
  // State is already marked dirty by addBridgeToState() calls
}

int getSlotLength(int slot, int flashOrLocal) {
  int slotLength = 0;
  if (flashOrLocal == 0) {
    nodeFile = safeFileOpen(("nodeFileSlot" + String(slot) + ".txt").c_str(), "r");
    while (nodeFile.available()) {
      nodeFile.read();
      slotLength++;
    }
    safeFileClose(nodeFile, false);
  } else {
    slotLength = nodeFileString.length();
  }

  return slotLength;
}

void printNodeFile(int slot, int printOrString, int flashOrLocal,
                   int definesInts, bool printEmpty, Stream *stream) {
  if (stream == nullptr) stream = &Jerial;

  if (stream == nullptr) stream = &Jerial;

  if (flashOrLocal == 0) {
    nodeFile = safeFileOpen(("nodeFileSlot" + String(slot) + ".txt").c_str(), "r");
    if (!nodeFile) {
      // if (debugFP)
      // stream->println("Failed to open nodeFile");
      return;
    } else {
      // if (debugFP)
      // stream->println("\n\ropened nodeFile.txt\n\n\rloading bridges from
      // file\n\r");
    }
    specialFunctionsString.clear();
    while (nodeFile.available() > 0) {
      specialFunctionsString += (char)nodeFile.read();
    }
    safeFileClose(nodeFile, false);
  } else {
    specialFunctionsString.clear();
    nodeFileString.printTo(specialFunctionsString);
    // specialFunctionsString.read(nodeFileString);
  }

  // Check if slot is empty and we don't want to print empty slots
  if (!printEmpty && isSlotFileEmpty(specialFunctionsString.c_str())) {
    return; // Don't print empty slots when printEmpty is false
  }

  //     int newLines = 0;
  // stream->println(specialFunctionsString.indexOf(","));
  // stream->println(specialFunctionsString.charAt(specialFunctionsString.indexOf(",")+1));
  // stream->println(specialFunctionsString.indexOf(","));
  if (debugFP == 0 || definesInts == 0) {
    // ... (rest of the mapping code)

    // specialFunctionsString.replace("116-80, 117-82, 114-83, 85-100, 81-100,",
    // "rotEnc_0,");

    specialFunctionsString.replace("100", "GND");
    specialFunctionsString.replace("101", "TOP_RAIL");
    specialFunctionsString.replace("102", "BOTTOM_RAIL");
    specialFunctionsString.replace("105", "5V");
    specialFunctionsString.replace("103", "3V3");
    specialFunctionsString.replace("106", "DAC0");
    specialFunctionsString.replace("107", "DAC1");
    specialFunctionsString.replace("108", "I_P");
    specialFunctionsString.replace("109", "I_N");
    specialFunctionsString.replace("110", "ADC0");
    specialFunctionsString.replace("111", "ADC1");
    specialFunctionsString.replace("112", "ADC2");
    specialFunctionsString.replace("113", "ADC3");
    specialFunctionsString.replace("114", "ADC4");
    specialFunctionsString.replace("115", "PROBE_MEASURE");
    specialFunctionsString.replace("116", "UART_TX");
    specialFunctionsString.replace("117", "UART_RX");
    specialFunctionsString.replace("118", "GPIO_18");
    specialFunctionsString.replace("119", "GPIO_19");
    specialFunctionsString.replace("120", "8V_P");
    specialFunctionsString.replace("121", "8V_N");
    specialFunctionsString.replace("70", "D0");
    specialFunctionsString.replace("71", "D1");
    specialFunctionsString.replace("72", "D2");
    specialFunctionsString.replace("73", "D3");
    specialFunctionsString.replace("74", "D4");
    specialFunctionsString.replace("75", "D5");
    specialFunctionsString.replace("76", "D6");
    specialFunctionsString.replace("77", "D7");
    specialFunctionsString.replace("78", "D8");
    specialFunctionsString.replace("79", "D9");
    specialFunctionsString.replace("80", "D10");
    specialFunctionsString.replace("81", "D11");
    specialFunctionsString.replace("82", "D12");
    specialFunctionsString.replace("83", "D13");
    specialFunctionsString.replace("84", "RESET");
    specialFunctionsString.replace("85", "AREF");
    specialFunctionsString.replace("86", "A0");
    specialFunctionsString.replace("87", "A1");
    specialFunctionsString.replace("88", "A2");
    specialFunctionsString.replace("89", "A3");
    specialFunctionsString.replace("90", "A4");
    specialFunctionsString.replace("91", "A5");
    specialFunctionsString.replace("92", "A6");
    specialFunctionsString.replace("93", "A7");
    specialFunctionsString.replace("94", "RESET_0");
    specialFunctionsString.replace("95", "RESET_1");
    // specialFunctionsString.replace("96", "GND_1");
    // specialFunctionsString.replace("97", "GND_0");
    // specialFunctionsString.replace("98", "3V3");
    // specialFunctionsString.replace("99", "5V");
    // specialFunctionsString.replace("128", "LOGO_PAD_TOP");
    // specialFunctionsString.replace("129", "LOGO_PAD_BOTTOM");
    // specialFunctionsString.replace("130", "GPIO_PAD");
    // specialFunctionsString.replace("131", "DAC_PAD");
    // specialFunctionsString.replace("132", "ADC_PAD");
    // specialFunctionsString.replace("133", "BUILDING_PAD_TOP");
    // specialFunctionsString.replace("134", "BUILDING_PAD_BOTTOM");
    // specialFunctionsString.replace("126", "BOTTOM_RAIL_GND");
    // specialFunctionsString.replace("104", "TOP_RAIL_GND");

    specialFunctionsString.replace("131", "GPIO_1");
    specialFunctionsString.replace("132", "GPIO_2");
    specialFunctionsString.replace("133", "GPIO_3");
    specialFunctionsString.replace("134", "GPIO_4");
    specialFunctionsString.replace("135", "GPIO_5");
    specialFunctionsString.replace("136", "GPIO_6");
    specialFunctionsString.replace("137", "GPIO_7");
    specialFunctionsString.replace("138", "GPIO_8");
    specialFunctionsString.replace("139", "BUFFER_IN");
    specialFunctionsString.replace("140", "BUFFER_OUT");
  }

  if (specialFunctionsString.charAt(specialFunctionsString.indexOf(",") + 1) !=
      '\n') {
    specialFunctionsString.replace(" ", "");
    specialFunctionsString.replace(",", ",\n\r");
    // specialFunctionsString.replace("{ ", "{\n\r");
    specialFunctionsString.replace("{", "{\n\r");
  }

  int specialFunctionsStringLength = specialFunctionsString.indexOf("}");
  if (specialFunctionsStringLength != -1) {
    if (specialFunctionsString.charAt(specialFunctionsStringLength + 1) !=
        '\n') {
      specialFunctionsString.replace("}", "}\n\r");
    }
    specialFunctionsString.remove(specialFunctionsStringLength + 2, -1);
  }

  // specialFunctionsString.readUntilToken(specialFunctionsString, "{");
  // specialFunctionsString.removeLast(9);
  // Jerial.print("*");
  if (printOrString == 0) {
    stream->println(specialFunctionsString);
    //     stream->println('*');
    // specialFunctionsString.clear();
  }
}


void clearNodeFile(int slot, int flashOrLocal) {
  // MIGRATED: Now uses globalState and SlotManager
  
  // Clear all connections in globalState
  globalState.clearAllConnections();
  
  // (Locked connections are infra functions now - re-added by
  // infraEvaluate() at the next rebuild; this inline clone is gone.)
  String errorMsg;
  
  if (flashOrLocal == 0) {
    // Save the cleared state to the slot
    SlotManager& mgr = SlotManager::getInstance();
    mgr.saveSlot(slot, errorMsg);
  }
  
  // // Clear net colors
  // clearChangedNetColors();
  // if (flashOrLocal == 0) {
  //   removeNetColorFile(slot);
  // } else {
  //   setSlotHasNetColors(slot, false);
  //   currentColorSlotColorsString.clear();
  // }
}

String slicedLines[130];
int slicedLinesIndex = 0;

// Global variables for storing last removed nodes
int lastRemovedNodes[20] = {-1};
int lastRemovedNodesIndex = 0;
bool disconnectedNodeNewData = false;

// int removeBridgeFromNodeFile(int node1, int node2, int slot, int flashOrLocal,
//                              int onlyCheck) {
//   // Reset the lastRemovedNodes buffer
//   lastRemovedNodesIndex = 0;
//   for (int i = 0; i < 20; i++) {
//     lastRemovedNodes[i] = -1;
//   }
//   disconnectedNodeNewData = false;

//   unsigned long timerStart = millis();
//   unsigned long timerEnd[5] = {0, 0, 0, 0, 0};


//   if (onlyCheck == 0) {
//   for (int i = 0; i < 8; i++) { // idk if I should do this here but YOLO
//     if (node1 == RP_GPIO_1 + i || node2 == RP_GPIO_1 + i) {
//       if (gpioNet[i] != -2) {
//         gpioNet[i] = -1;
//       }
//     }
//   }
//   }
//   // Jerial.print("Slot = ");
//   // Jerial.println(slot);
//   if (flashOrLocal == 0) {

//     openFileThreadSafe(rplus, slot);

//     if (!nodeFile) {
//       if (debugFP) {
//         // Jerial.println("Failed to open nodeFile (removeBridgeFromNodeFile)");
//       }

//       return -1;
//     } else {
//       if (debugFP)
//         Jerial.println(
//             "\n\ropened nodeFile.txt\n\n\rloading bridges from file\n\r");
//     }

//     if (nodeFile.size() < 2) {
//       // Jerial.println("empty file");
//       nodeFile.close();
//       core1busy = false;
//       return -1;
//     }
//     nodeFile.seek(0);
//     nodeFile.setTimeout(8);
//   }
//   if (onlyCheck == 1) {
//     // Jerial.print("Checking for bridge between ");
//     // Jerial.print(node1);
//     // Jerial.print(" and ");
//     // Jerial.println(node2);
//   }
//   timerEnd[0] = millis() - timerStart;

//   for (int i = 0; i < 120; i++) {
//     slicedLines[i] = " ";
//   }
//   slicedLinesIndex = 0;
//   int numberOfLines = 0;
//   // nodeFileString.clear();
//   String lineBufString = "";
//   // nodeFileString.printTo(Serial);
//   // Jerial.println(" ");
//   // Jerial.print("nodeFileString = ");
//   // Jerial.println(nodeFileString);
//   // core1busy = true;
//   createSafeString(lineBufSafe, 40);
//   int lineIdx = 0;
//   int charIdx = 0;
//   for (int i = 0; i < 120; i++) {
//     slicedLines[lineIdx] = " ";

//     if (flashOrLocal == 0) {
//       lineBufString = nodeFile.readStringUntil(',');

//     } else {
//       charIdx = nodeFileString.stoken(lineBufSafe, charIdx, ",");
//       lineBufString = lineBufSafe.c_str();
//       if (charIdx == -1) {
//         numberOfLines = lineIdx;
//         // Jerial.print ("\n\r\t\t\t\tt\t\tnumberOfLines = ");
//         // Jerial.println(numberOfLines);
//         //  Jerial.println("end of file char idx");

//         break;
//       }
//     }
//     // Jerial.print("lineBufSafe = ");
//     // Jerial.println(lineBufSafe);
//     // Jerial.print("lineBufString = ");
//     // Jerial.println(lineBufString);

//     lineBufString.trim();
//     lineBufString.replace(" ", "");
//     // Jerial.print("$");
//     // Jerial.print(lineBufString);
//     // Jerial.println("$");

//     if (lineBufString.length() < 3 || lineBufString == " ") {
//       numberOfLines = lineIdx;
//       // Jerial.print ("numberOfLines = ");
//       // Jerial.println(numberOfLines);
//       //  Jerial.println("end of file");

//       break;
//     }
//     slicedLines[lineIdx].concat(lineBufString);

//     // Jerial.print("#");
//     // Jerial.print(slicedLines[lineIdx]);
//     // Jerial.println("#");
//     slicedLines[lineIdx].replace("\n", "");
//     slicedLines[lineIdx].replace("\r", "");

//     slicedLines[lineIdx].replace("{", "");
//     slicedLines[lineIdx].replace("}", "");
//     slicedLines[lineIdx].replace(",", "");
//     // slicedLines[lineIdx].trim();
//     slicedLines[lineIdx].replace("-", " - ");
//     slicedLines[lineIdx].concat(" , ");
//     // Jerial.print("*");
//     // Jerial.print(slicedLines[lineIdx]);
//     // Jerial.println("*");

//     lineIdx++;
//   }
//   timerEnd[1] = millis() - timerStart;
//   numberOfLines = lineIdx;
//   // Jerial.print("numberOfLines = ");
//   // Jerial.println(numberOfLines);
//   // Jerial.print("nodeFileString = ");
//   // Jerial.println(nodeFileString);
//   // nodeFileString.clear();
//   // nodeFile.close();
//   // Jerial.println(nodeFileString);
//   // Jerial.print("lineIdx = ");
//   // Jerial.println(lineIdx);
//   char nodeAsChar[40];
//   itoa(node1, nodeAsChar, 10);
//   String paddedNode1 = " ";
//   String paddedNode2 = " ";

//   paddedNode1.concat(nodeAsChar);
//   paddedNode1.concat(" ");
//   // Jerial.print("paddedNode1 = *");
//   // Jerial.print(paddedNode1);
//   // Jerial.println("*");

//   if (node2 != -1) {
//     itoa(node2, nodeAsChar, 10);

//     paddedNode2.concat(nodeAsChar);
//     paddedNode2.concat(" ");
//     // Jerial.print("paddedNode2 = *");
//     // Jerial.print(paddedNode2);
//     // Jerial.println("*");
//   }

//   // nodeFile.truncate(0);

//   timerEnd[2] = millis() - timerStart;

//   if (flashOrLocal == 0) {
//     nodeFile.close();
//     openFileThreadSafe(w, slot);
//     // nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "w");
//     nodeFile.print("{");
//   } else {
//     nodeFileString.clear();
//     nodeFileString.concat("{ ");
//   }
//   // nodeFile.print(" { \n\r");
//   // Jerial.print("numberOfLines = ");
//   // Jerial.println(numberOfLines);
//   int removedLines = 0;

//   for (int i = 0; i < numberOfLines; i++) {
//     // Jerial.print("\n\rslicedLines[");

//     // Jerial.print(i);
//     // Jerial.print("] = ");
//     // Jerial.println(slicedLines[i]);
//     // delay(10);
//     // Jerial.println(millis()-timerStart);
//     int remove = 0;

//     if (node2 == -1 && slicedLines[i].indexOf(paddedNode1) != -1) {
//       if (onlyCheck == 0) {
//         remove = 1;

//         // Extract the other node in this connection
//         String lineStr = slicedLines[i];
//         lineStr.replace(" ", "");
//         int dashPos = lineStr.indexOf("-");

//         if (dashPos != -1) {
//           String node1Str = lineStr.substring(0, dashPos);
//           String node2Str = lineStr.substring(dashPos + 1);

//           int nodeA = node1Str.toInt();
//           int nodeB = node2Str.toInt();

//           // Add the other node to our list
//           int otherNode = (nodeA == node1) ? nodeB : nodeA;

//           // Store in the buffer if we have space
//           if (lastRemovedNodesIndex < 20 && otherNode > 0) {
//             lastRemovedNodes[lastRemovedNodesIndex++] = otherNode;
//             disconnectedNodeNewData = true;
//           }
//         }
//       }

//       removedLines++;
//     } else if (slicedLines[i].indexOf(paddedNode1) != -1 &&
//                slicedLines[i].indexOf(paddedNode2) != -1) {
//       if (onlyCheck == 0) {
//         remove = 1;

//         // When removing a specific connection, add the other node
//         if (lastRemovedNodesIndex < 20) {
//           lastRemovedNodes[lastRemovedNodesIndex++] = node2;
//           disconnectedNodeNewData = true;
//         }
//       }

//       removedLines++;
//     }
//     if (remove == 0) {
//       remove = 0;
//       slicedLines[i].replace(" ", "");

//       if (flashOrLocal == 0) {
//         nodeFile.print(slicedLines[i]);
//       } else {
//         // nodeFileString.concat(slicedLines[i]);

//         for (int j = 0; j < 39; j++) {

//           nodeAsChar[j] = ' ';
//         }
//         // Jerial.print("nodeAsChar1 = ");
//         // Jerial.println(nodeAsChar);

//         slicedLines[i].toCharArray(nodeAsChar, 40);
//         // Jerial.print("nodeAsChar2 = *");
//         // Jerial.print(nodeAsChar);
//         // Jerial.println("*");
//         slicedLines[i].replace(",", "");
//         slicedLines[i].replace(" ", "");
//         slicedLines[i].concat(",");
//         nodeFileString.concat(nodeAsChar);
//         nodeFileString.replace(" ", "");
//         //     Jerial.print("sliceLines[i].length() = ");
//         //     Jerial.println(slicedLines[i].length());
//         //         Jerial.print("nodeFileString = ");
//         // Jerial.println(nodeFileString);
//         nodeFileString.replace("{ ", "");
//         nodeFileString.replace(" } ", "");
//         nodeFileString.replace("{", "");
//         nodeFileString.replace("}", "");
//         nodeFileString.prefix("{ ");
//         // nodeFileString.concat(" } ");
//         //  Jerial.print("nodeFileString = ");
//         //  Jerial.println(nodeFileString);
//       }
//     }
//     //     Jerial.print("slicedLines[");
//     //       Jerial.print(i);
//     //       Jerial.print("] = ");
//     //  Jerial.println(slicedLines[i]);
//   }

//   if (flashOrLocal == 0) {
//     nodeFile.print(" } ");
//     nodeFile.close();
//     if (onlyCheck == 0) { // Only mark as modified if we actually changed something
//       markSlotAsModified(slot);
//     }
//   } else {
//     nodeFileString.concat(" } ");
//     // Jerial.print("nodeFileString = ");
//     // Jerial.println(nodeFileString);
//   }

//   // for (int i = 0; i <= numberOfLines; i++) {
//   //   Jerial.print("\n\rslicedLines[");
//   //   Jerial.print(i);
//   //   Jerial.print("] = ");
//   //   Jerial.println(slicedLines[i]);

//   // }
//   core1busy = false;

//   if (onlyCheck == 0) {
//     removeChangedNetColors(node1, 1);
//     if (node2 != -1) {
//       removeChangedNetColors(node2, 0);
//     }
//   }

//   timerEnd[3] = millis() - timerStart;
//   // Jerial.print("timerEnd[0] = ");

//   //   Jerial.println(timerEnd[0]);
//   //   Jerial.print("timerEnd[1] = ");
//   //   Jerial.println(timerEnd[1]);
//   //   Jerial.print("timerEnd[3] = ");
//   //   Jerial.println(timerEnd[3]);
//   return removedLines;
// }

// int addBridgeToNodeFile(int node1, int node2, int slot, int flashOrLocal,
//                         int allowDuplicates) {

//   // Jerial.print("nodeFileStringAdd = ");
//   // Jerial.println(nodeFileString);
//   unsigned long timerStart[5];
//   timerStart[0] = micros();
//   if (flashOrLocal == 0) {
//     // nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "r+");

//     openFileThreadSafe(rplus, slot);
//     // Jerial.println(nodeFile);
//     // Jerial.print("Slot = ");
//     // Jerial.println(slot);
//     if (!nodeFile) {
//       // if (debugFP) {
//       // Jerial.println("Failed to open nodeFile (addBridgeToNodeFile)");
//       //  }
//       // reateSlots(slot, 0);
//       //  delay(10);
//       //  nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt",
//       //  "w+"); nodeFile.print("{ ");
//       openFileThreadSafe(w, slot);
//       nodeFile.print("{ } ");
//       // return;
//     } else {
//       if (debugFP)
//         Jerial.println(
//             "\n\ropened nodeFile.txt\n\n\rloading bridges from file\n\r");
//     }
//     //     while (nodeFile.available()) {
//     //     Jerial.write(nodeFile.read());
//     //   }
//     //   nodeFile.seek(0);
//     nodeFile.setTimeout(15);
//   }

//   // Jerial.print("flashOrLocal = ");
//   // Jerial.println(flashOrLocal);

//   for (int i = 0; i < 120; i++) {
//     slicedLines[i] = " ";
//   }
//   timerStart[1] = micros();

//   int numberOfLines = 0;
//   // Jerial.print("nodeFileString = ");
//   // Jerial.println(nodeFileString);
//   // nodeFileString.printTo(Serial);
//   // Jerial.println(" ");
//   String lineBufString = "";

//   createSafeString(lineBufSafe, 30);
//   int lineIdx = 0;
//   int charIdx = 0;
//   nodeFileString.trim();

//   for (int i = 0; i < 120; i++) {

//     slicedLines[lineIdx] = " ";
//     // if (i == 0 && nodeFileString.startsWith("{") == -1 && flashOrLocal == 1)
//     // {
//     //   slicedLines[0].concat("{");

//     // }

//     if (flashOrLocal == 0) {
//       lineBufString = nodeFile.readStringUntil(',');

//     } else {
//       charIdx = nodeFileString.stoken(lineBufSafe, charIdx, ",");
//       // if (charIdx == -1 || lineBufSafe == "}") {
//       //   numberOfLines = lineIdx;
//       //   break;
//       // }
//       lineBufString = lineBufSafe.c_str();
//     }

//     // Jerial.print("lineBufSafe = ");
//     // Jerial.println(lineBufSafe);
//     // Jerial.print("lineBufString = ");
//     // Jerial.println(lineBufString);

//     lineBufString.trim();
//     lineBufString.replace(" ", "");

//     if (lineBufString.length() < 3 || lineBufString == " ") {
//       // Jerial.println("end of file");
//       numberOfLines = lineIdx;
//       break;
//     }
//     slicedLines[lineIdx].concat(lineBufString);

//     slicedLines[lineIdx].replace("\n", "");
//     slicedLines[lineIdx].replace("\r", "");

//     slicedLines[lineIdx].replace("{", "");
//     slicedLines[lineIdx].replace("}", "");
//     slicedLines[lineIdx].replace(",", "");
//     slicedLines[lineIdx].concat(",");

//     // Jerial.print("lineBufString = ");
//     // Jerial.println(lineBufString);
//     // slicedLines[lineIdx].trim();

//     // Jerial.print("*");
//     // Jerial.print(slicedLines[lineIdx]);
//     // Jerial.println("*");

//     lineIdx++;
//   }
//   timerStart[2] = micros();
//   numberOfLines = lineIdx;

//   // Jerial.print("\n\rnumberOfLines = ");
//   // Jerial.println(numberOfLines);

//   // nodeFileString.clear();
//   // nodeFile.close();
//   // Jerial.println(nodeFileString);

//   char nodeAsChar[40];
//   itoa(node1, nodeAsChar, 10);
//   String addNode1 = "";
//   String addNode2 = "";

//   addNode1.concat(nodeAsChar);
//   addNode1.concat("-");

//   itoa(node2, nodeAsChar, 10);

//   addNode2.concat(nodeAsChar);
//   addNode2.concat(",");

//   addNode1.concat(addNode2);

//   addNode1.toCharArray(nodeAsChar, addNode1.length() + 1);
//   nodeAsChar[addNode1.length()] = '\0';

//   // createSafeString(addNodeSafe, 40);

//   int duplicateFound = 0;

//   if (flashOrLocal == 0) {
//     // Jerial.println("flash");
//     openFileThreadSafe(wplus, slot);
//     // nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "w+");
//     nodeFile.print("{ ");

//     for (int i = 0; i < numberOfLines; i++) {

//       if (slicedLines[i].indexOf(addNode1) != -1) {
//         duplicateFound = 1;
//         // Jerial.println("Duplicate found (flash)");
//       }

//       nodeFile.print(slicedLines[i]);
//     }

//     if (duplicateFound == 0 || allowDuplicates == 1) {
//       nodeFile.print(addNode1);
//     }
//     nodeFile.print(" } ");
//     // nodeFile.seek(0);

//     nodeFile.close();
//     markSlotAsModified(slot); // Mark slot as needing re-validation

//   } else {
//     //  Jerial.println("local");
//     // Jerial.print("nodeFileString1 = ");
//     // Jerial.println(nodeFileString);
//     // Jerial.print("addNode1 = ");
//     // Jerial.println(addNode1);

//     if (nodeFileString.indexOf(nodeAsChar, 0) != -1) {
//       duplicateFound = 1;
//       // Jerial.println("Duplicate found (local)");
//     }

//     nodeFileString.replace('\n', "");
//     nodeFileString.replace('\r', "");
//     nodeFileString.replace(' ', "");
//     nodeFileString.replace('{', "");
//     nodeFileString.replace('}', "");

//     // Jerial.print("\n\n\rduplicateFound = ");
//     //     Jerial.println(duplicateFound);
//     //     Jerial.print("nodeAsChar = ");
//     //     Jerial.println(nodeAsChar);

//     if (duplicateFound == 0 || allowDuplicates == 1) {
//       // nodeFileString.concat(addNode1);
//       nodeFileString.concat(nodeAsChar, addNode1.length());
//     }

//     nodeFileString.prefix("{ ");
//     nodeFileString.concat(" } ");

//     //     Jerial.print("nodeFileStringAdd = ");
//     // Jerial.println(nodeFileString);
//   }
//   timerStart[3] = micros();

//   // for (int i = 0; i < 4; i++) {
//   //   Jerial.print("timerStart[");
//   //   Jerial.print(i);
//   //   Jerial.print("] = ");
//   //   Jerial.println(timerStart[i] - timerStart[0]);
//   // }
//   return duplicateFound;
// }

createSafeString(serialString, 100);
createSafeString(dash, 2);

createSafeString(comma, 2);


void readStringFromSerial(int source, int addRemove) {

  int node1 = 0;
  int node2 = 0;
  int numberOfBridges = 0;
  int finished = 1;
  int singleNode = 0;

  specialFunctionsString.clear();
  serialString.clear();
  // dash.clear();
  // comma.clear();
  // dash.concat("-");
  // comma.concat(",");
  if (source == 0) {
    specialFunctionsString.read(Jerial);
  } else if (source == 1) {
   // specialFunctionsString.read(Serial1);
  } else if (source == 3) {
    // Read from global command line storage (without first character for backwards compatibility)
    String lineData = currentCommandLine;
    if (lineData.length() > 0) {
      lineData = lineData.substring(1);  // Remove first character
    }
    // Serial.println("lineData = ");
    // Serial.println(lineData);
    specialFunctionsString.clear();
    specialFunctionsString = lineData.c_str();
  }

  if (specialFunctionsString.endsWith("-") == 1 ||
      specialFunctionsString.endsWith(";") == 1 ||
      specialFunctionsString.endsWith("}") == 1 ||
      specialFunctionsString.endsWith("[") == 1) {
    specialFunctionsString.removeLast(1);
  }

  if (specialFunctionsString.startsWith("{") == 1 ||
      specialFunctionsString.startsWith("f") == 1 ||
      specialFunctionsString.startsWith("-") == 1 ||
      specialFunctionsString.startsWith("[") == 1) {
    specialFunctionsString.removeBefore(1);
  }

  replaceSFNamesWithDefinedInts();
  replaceNanoNamesWithDefinedInts();

  do {
    // nodeFileString.clear();
    // nodeFileString.read(Serial);
    int dashIndex = specialFunctionsString.indexOf("-");
    if (dashIndex == -1 && addRemove == 0) {
      Jerial.println("Invalid input");
      return;
    } else if (dashIndex == -1 && addRemove == 1) {
      singleNode = 1;
      node2 = -1;
      finished = 1;
      dashIndex = specialFunctionsString.length();
    }

    specialFunctionsString.substring(serialString, 0, dashIndex);
    // serialString.readUntil(specialFunctionsString, "-");
    // serialString.removeLast(1);
    serialString.toInt(node1);
    // serialString.printTo(Serial);
    // Jerial.println();
    // nodeFileString.printTo(Serial);
    //  Jerial.println();
    // nodeFileString.clear();
    serialString.clear();

    if (singleNode != 1) {
      int commaIndex = specialFunctionsString.indexOf(",");
      if (commaIndex != -1) {
        specialFunctionsString.substring(serialString, dashIndex + 1,
                                         commaIndex);
      } else {
        commaIndex = specialFunctionsString.length();
        specialFunctionsString.substring(serialString, dashIndex + 1,
                                         commaIndex);
      }

      // specialFunctionsString.printTo(Serial);
      // Jerial.println();

      if (specialFunctionsString.indexOfCharFrom("-", dashIndex + 1) != -1) {
        specialFunctionsString.removeBefore(commaIndex + 1);
        // specialFunctionsString.substring(specialFunctionsString, commaIndex +
        // 1, specialFunctionsString.length());
        finished = 0;
      } else {
        finished = 1;
      }

      // specialFunctionsString.printTo(Serial);
      // Jerial.println();

      serialString.toInt(node2);
      // serialString.printTo(Serial);
      // Jerial.println();
      // Jerial.print("node1 = ");
      // Jerial.println(node1);
      // Jerial.print("node2 = ");
      // Jerial.println(node2);



    }

    if (isNodeValid(node1) != 1) {
      Jerial.println("Invalid node 1 number");
      return;
    }

    if (isNodeValid(node2) != 1 && (addRemove == 0)) {
      Jerial.println("Invalid node 2 number");
      return;
    }

    if (addRemove == 0) {
      addBridgeToState(node1, node2);
      //saveStateToSlot();  // Save after each addition
    } else if (addRemove == 1) {
      if (node1 == node2 || isNodeValid(node2) == 0) {
        node2 = -1;
      }

      removeBridgeFromState(node1, node2);
      //saveStateToSlot();  // Save after each removal
    }

  } while (finished == 0);
  printNodeFile(netSlot, 0, 0, 0, true, &Jerial);
}

int parseStringToNode(int source) { return 0; }

bool isNodeOnBoard(int node) {
  const auto &b = board::currentBoard();
  for (int c = 0; c < board::kChipCount; c++) {
    for (int x = 0; x < board::kXLanes; x++) {
      if (b.xMap[c][x] == node) {
        return true;
      }
    }
    for (int y = 0; y < board::kYLanes; y++) {
      if (b.yMap[c][y] == node) {
        return true;
      }
    }
  }
  return false;
}

int isNodeValid(int node) {
  if (node == -1) {
    return -1;
  }
  
  bool withinRange = false;
  if (node >= 1 && node <= 60) {
    withinRange = true;
  } else if (node >= 70 && node <= 93) {
    withinRange = true;
  } else if (node >= 100 && node <= 117) {
    withinRange = true;
  } else if (node >= RP_GPIO_1 && node <= ROUTABLE_BUFFER_OUT) {
    withinRange = true;
  } else if (node >= FAKE_GPIO_1 && node <= FAKE_GPIO_32) {
    withinRange = true;
  }
  
  if (!withinRange) {
    return 0;
  }
  
  // Breadboard rows, GND, and fake GPIOs are always valid
  if ((node >= 1 && node <= 60) || node == GND || (node >= FAKE_GPIO_1 && node <= FAKE_GPIO_32)) {
    return 1;
  }
  
  // For other nodes, verify they physically exist on the active board
  if (isNodeOnBoard(node)) {
    return 1;
  }
  
  // Print warning if enabled
  if (jumperlessConfig.debug.show_node_errors) {
    Jerial.printf("Node %d (%s) doesn't exist on this hardware\n\r", node, definesToChar(node, 0));
  }
  
  return 0;
}

// Lightning-fast validation using character parsing instead of String operations
int validateNodeFileFast(const char* content, int contentLen, bool verbose) {
  if (contentLen < 4) {
    if (verbose) Jerial.println("◇ Content too short");
    return 1;
  }

  // Find braces using simple character scanning
  int openBrace = -1, closeBrace = -1;
  for (int i = 0; i < contentLen; i++) {
    if (content[i] == '{' && openBrace == -1) openBrace = i;
    else if (content[i] == '}') closeBrace = i;
  }

  if (openBrace == -1) {
    if (verbose) Jerial.println("◇ Missing opening brace");
    return 2;
  }
  if (closeBrace == -1 || closeBrace <= openBrace) {
    if (verbose) Jerial.println("◇ Missing/invalid closing brace");
    return 3;
  }

  // Fast validation: check basic structure only
  int connectionCount = 0;
  bool inNumber = false;
  bool foundDash = false;
  int node1 = 0, node2 = 0;
  bool valid = true;

  for (int i = openBrace + 1; i < closeBrace; i++) {
    char c = content[i];
    
    if (c >= '0' && c <= '9') {
      if (!inNumber) {
        inNumber = true;
        if (!foundDash) node1 = node1 * 10 + (c - '0');
        else node2 = node2 * 10 + (c - '0');
      } else {
        if (!foundDash) node1 = node1 * 10 + (c - '0');
        else node2 = node2 * 10 + (c - '0');
      }
    } else if (c == '-') {
      if (!inNumber || foundDash) {
        valid = false;
        break;
      }
      foundDash = true;
      inNumber = false;
    } else if (c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (foundDash && inNumber) {
        // End of a connection - quick validate
        if (isNodeValid(node1) != 1 || isNodeValid(node2) != 1) {
          valid = false;
          break;
        }
        connectionCount++;
        node1 = node2 = 0;
        foundDash = false;
      }
      inNumber = false;
    } else if (c != ' ') {
      // Invalid character
      valid = false;
      break;
    }
  }

  // Handle last connection if no trailing comma
  if (valid && foundDash && inNumber) {
    if (isNodeValid(node1) == 1 && isNodeValid(node2) == 1) {
      connectionCount++;
    } else {
      valid = false;
    }
  }

  if (verbose) {
    Jerial.print("◆ Fast validation: ");
    Jerial.print(connectionCount);
    Jerial.print(" connections, ");
    Jerial.println(valid ? "VALID" : "INVALID");
  }

  return valid ? 0 : 5;
}

// Keep the old validation for comparison/fallback if needed
int validateNodeFile(const String &content, bool verbose) {
  // Use fast validation instead of slow String operations
  return validateNodeFileFast(content.c_str(), content.length(), verbose);
}

int validateNodeFileSlot(int slot, bool verbose) {
  // Fast validation using direct file reading instead of String operations
  String filename = "nodeFileSlot" + String(slot) + ".txt";

  if (verbose) {
    Jerial.println("◆ Fast validating " + filename + "...");
  }

  if (!safeFileExists(filename.c_str(), 500)) {
    if (verbose)
      Jerial.println("◇ Slot file does not exist");
    return 1; // File doesn't exist, treat as empty
  }

  File slotFile = safeFileOpen(filename.c_str(), "r", 1000);
  if (!slotFile) {
    if (verbose)
      Jerial.println("◇ Failed to open slot file");
    return 1;
  }

  // Read file efficiently into a small buffer
  size_t fileSize = slotFile.size();
  if (fileSize > 512) {
    // File too large, probably corrupted
    safeFileClose(slotFile, false);
    if (verbose) Jerial.println("◇ File too large");
    return 5;
  }

  char buffer[513]; // Small stack buffer
  size_t bytesRead = slotFile.readBytes(buffer, min(fileSize, (size_t)512));
  buffer[bytesRead] = '\0'; // Null terminate
  safeFileClose(slotFile, false);

  return validateNodeFileFast(buffer, bytesRead, verbose);
}

const char *getNodeFileValidationError(int errorCode) {
  switch (errorCode) {
  case 0:
    return "Valid";
  case 1:
    return "Empty or missing content";
  case 2:
    return "Missing opening brace '{'";
  case 3:
    return "Missing closing brace '}'";
  case 4:
    return "Invalid node number found";
  case 5:
    return "Malformed connection format";
  case 6:
    return "Invalid connection format";
  default:
    return "Unknown error";
  }
}

bool isSlotFileEmpty(const String &content) {
  // Check if the content is just empty braces after stripping whitespace
  String trimmed = content;
  trimmed.trim();
  trimmed.replace(" ", "");
  trimmed.replace("\n", "");
  trimmed.replace("\r", "");
  trimmed.replace("\t", "");

  return (trimmed == "{}" || trimmed == "{ }" || trimmed.length() < 4);
}

bool isSlotFileEmpty(int slot) {
  String content = readSlotFileContent(slot);
  return isSlotFileEmpty(content);
}



void openNodeFile(int slot, int flashOrLocal) {
  timeToFP = millis();
  netsUpdated = false;

  // Ultra-fast validation check: only validate if not already validated (only for flash files)
  if (flashOrLocal == 0) {
    if (!slotIsValidated(slot)) {
      if (debugFP) {
        Jerial.println("◆ Ultra-fast validating nodeFileSlot" + String(slot) + ".txt...");
      }
      
      // Do minimal validation - just check if file exists and has basic structure
      String slotFilename = "nodeFileSlot" + String(slot) + ".txt";
      if (safeFileExists(slotFilename.c_str())) {
        File quickCheck = safeFileOpen(slotFilename.c_str(), "r");
        if (quickCheck) {
          size_t fileSize = quickCheck.size();

          // For very small files, check everything
          if (fileSize <= 64) {
            bool hasOpenBrace = false, hasCloseBrace = false;
            char buffer[65];
            int bytesRead = quickCheck.readBytes(buffer, fileSize);
            buffer[bytesRead] = '\0';

            for (int i = 0; i < bytesRead; i++) {
              if (buffer[i] == '{') hasOpenBrace = true;
              if (buffer[i] == '}') hasCloseBrace = true;
            }

            // Only fix if actually missing braces in small files
            if (!hasOpenBrace || !hasCloseBrace) {
              safeFileClose(quickCheck, false);
              if (debugFP) {
                Jerial.println("◇ Small file missing braces, fixing");
              }
              File fixFile = safeFileOpen(slotFilename.c_str(), "w");
              if (fixFile) {
                const char* emptyJson = "{ }";
                fixFile.write((const uint8_t*)emptyJson, strlen(emptyJson));
                safeFileClose(fixFile, true);
              }
            } else {
              safeFileClose(quickCheck, false);
            }
          } else {
            // For larger files, check beginning and end for braces
            bool hasOpenBrace = false, hasCloseBrace = false;
            char startBuffer[32], endBuffer[32];

            // Check beginning for opening brace
            int startRead = quickCheck.readBytes(startBuffer, 31);
            for (int i = 0; i < startRead; i++) {
              if (startBuffer[i] == '{') {
                hasOpenBrace = true;
                break;
              }
            }

            // Check end for closing brace
            if (fileSize > 31) {
              quickCheck.seek(fileSize - 31);
              int endRead = quickCheck.readBytes(endBuffer, 31);
              for (int i = 0; i < endRead; i++) {
                if (endBuffer[i] == '}') {
                  hasCloseBrace = true;
                  break;
                }
              }
            }

            safeFileClose(quickCheck, false);

            // For larger files, assume they're probably OK even if we don't find braces
            // (they might be in the middle section we didn't read)
            if (debugFP && (!hasOpenBrace || !hasCloseBrace)) {
              Jerial.println("◇ Large file, braces not found in start/end - assuming OK");
            }
          }
        }
      } else {
        // File doesn't exist, create empty one
        if (debugFP) {
          Jerial.println("◇ File doesn't exist, creating empty file");
        }
        File createFile = safeFileOpen(("nodeFileSlot" + String(slot) + ".txt").c_str(), "w");
        if (createFile) {
          const char* emptyJson = "{ }";
          createFile.write((const uint8_t*)emptyJson, strlen(emptyJson));
          safeFileClose(createFile, true);
        }
      }
      
      // Mark as validated after our quick check
      setSlotValidated(slot, true);
      if (debugFP) {
        Jerial.println("◆ Slot " + String(slot) + " ultra-fast validated and cached");
      }
    } else {
      if (debugFP) {
        Jerial.println("◆ Slot " + String(slot) + " already validated (skipping)");
      }
    }
  }

  // Jerial.println(nodeFileString);
  // Jerial.println("opening nodeFileSlot" + String(slot) + ".txt");
  // Jerial.println("flashOrLocal = " + String(flashOrLocal));

  // Jerial.println(flashOrLocal?"local":"flash");

  // while (core2busy == true) {
  //   // Jerial.println("waiting for core2 to finish");
  // }
  // core1busy = true;
  // Save/restore rather than set/clear: this loader can run nested under an
  // outer core1busy holder (the refresh paths raise it around the whole
  // netlist rebuild), and the old unconditional clear at the end dropped the
  // OUTER claim mid-rebuild - reopening the gates that NetVoltageScan,
  // OledGui, and systemIdleForFlush key off while chipStates was still torn.
  bool wasCore1Busy = core1busy;
  if ((nodeFileString.length() < 3 && flashOrLocal == 1) || flashOrLocal == 0) {

    // Hold fs_mutex across the whole open->read->close so nothing else (the
    // USB-MSC mount callback closing nodeFile from tud_task on the other
    // core, or any other fs user) can yank the global nodeFile out from
    // under the read loop. fs_mutex is per-core recursive, so the
    // acquire/release inside openFileThreadSafe() just nests within this
    // one. core1busy pairs with the restore at the end of this function.
    fs_mutex_acquire();
    core1busy = true;

    // nodeFile = FatFS.open("nodeFileSlot" + String(slot) + ".txt", "r");
    openFileThreadSafe(r, slot);
    
    if (!nodeFile) {
      if (debugFP)
        Jerial.println("Failed to open nodeFile");

      // createSlots(slot, 0);
      openFileThreadSafe(w, slot);
      if (nodeFile) {
        const char* emptyJson = "{ } ";
        nodeFile.write((const uint8_t*)emptyJson, strlen(emptyJson));
      }
      // core1busy = false;
      // return;
    } else {
      if (debugFP)
        Jerial.println("\n\ropened nodeFileSlot" + String(slot) +
                       +".txt\n\n\rloading bridges from file\n\r");
    }

    nodeFileString.clear();
    while (nodeFile.available() > 0) {
      nodeFileString += (char)nodeFile.read();
    }
    // delay(10);
    // Jerial.println(nodeFileString);

    nodeFile.close();

    fs_mutex_release();

    // multicore_lockout_end_blocking();
  }

  // Additional validation of the loaded content before parsing
  // Only validate if this slot hasn't been validated yet (skip if already validated)
  if (flashOrLocal == 0 && !slotIsValidated(slot)) {
    int validation_result = validateNodeFile(nodeFileString.c_str(), false);
    if (validation_result != 0) {
      if (debugFP) {
        Jerial.println("◇ Loaded content failed validation, clearing and using "
                       "empty file");
      }
      nodeFileString.clear();
      nodeFileString.concat("{ }");
      clearNodeFile(slot, 0);
    }
  }

  // Jerial.println(nodeFileString);
  // Jerial.println();
  //   Jerial.println("nodeFileString = ");
  // nodeFileString.printTo(Serial);

  splitStringToFields();

  core1busy = wasCore1Busy;
  // parseStringToBridges();
}

void splitStringToFields() {
  int openBraceIndex = 0;
  int closeBraceIndex = 0;

  if (debugFP)
    Jerial.println("\n\rraw input file\n\r");
  if (debugFP)
    Jerial.println(nodeFileString);
  if (debugFP)
    Jerial.println("\n\rsplitting and cleaning up string\n\r");
  if (debugFP)
    Jerial.println("_");

  openBraceIndex = nodeFileString.indexOf("{");
  closeBraceIndex = nodeFileString.indexOf("}");
  int fIndex = nodeFileString.indexOf("f");

  int foundOpenBrace = -1;
  int foundCloseBrace = -1;
  int foundF = -1;

  if (openBraceIndex != -1) {
    foundOpenBrace = 1;
  }
  if (closeBraceIndex != -1) {
    foundCloseBrace = 1;
  }
  if (fIndex != -1) {
    foundF = 1;
  }

  // Jerial.println(openBraceIndex);
  // Jerial.println(closeBraceIndex);
  // Jerial.println(fIndex);

  if (foundF == 1) {
    nodeFileString.substring(nodeFileString, fIndex + 1,
                             nodeFileString.length());
  }

  if (foundOpenBrace == 1 && foundCloseBrace == 1) {

    nodeFileString.substring(specialFunctionsString, openBraceIndex + 1,
                             closeBraceIndex);
  } else {
    nodeFileString.substring(specialFunctionsString, 0,
                             -1); // nodeFileString.length());
  }
  specialFunctionsString.trim();

  if (debugFP)
    Jerial.println(specialFunctionsString);

  if (debugFP)
    Jerial.println("^\n\r");
  /*
      nodeFileString.remove(0, closeBraceIndex + 1);
      nodeFileString.trim();

      openBraceIndex = nodeFileString.indexOf("{");
      closeBraceIndex = nodeFileString.indexOf("}");
      //nodeFileString.substring(specialFunctionsString, openBraceIndex + 1,
     closeBraceIndex); specialFunctionsString.trim();
      if(debugFP)Jerial.println("_");
      if(debugFP)Jerial.println(specialFunctionsString);
      if(debugFP)Jerial.println("^\n\r");
      */
  replaceSFNamesWithDefinedInts();
  replaceNanoNamesWithDefinedInts();
  parseStringToBridges();
}

void replaceSFNamesWithDefinedInts(void) {
  specialFunctionsString.toUpperCase();
  if (debugFP) {
    Jerial.println("replacing special function names with defined ints\n\r");
    Jerial.println(specialFunctionsString);
  }

  specialFunctionsString.replace("GND", "100");
  specialFunctionsString.replace("GROUND", "100");
  specialFunctionsString.replace("TOP_RAIL", "101");
  specialFunctionsString.replace("TOPRAIL", "101");
  specialFunctionsString.replace("T_R", "101");
  specialFunctionsString.replace("TOP_R", "101");
  specialFunctionsString.replace("BOTTOM_RAIL", "102");
  specialFunctionsString.replace("BOT_RAIL", "102");
  specialFunctionsString.replace("BOTTOMRAIL", "102");
  specialFunctionsString.replace("BOTRAIL", "102");
  specialFunctionsString.replace("B_R", "102");
  specialFunctionsString.replace("BOT_R", "102");

  specialFunctionsString.replace("SUPPLY_5V", "105");
  specialFunctionsString.replace("SUPPLY_3V3", "103");

  specialFunctionsString.replace("DAC0_5V", "106");
  specialFunctionsString.replace("DAC1_8V", "107");
  specialFunctionsString.replace("DAC0", "106");
  specialFunctionsString.replace("DAC1", "107");
  specialFunctionsString.replace("DAC_0", "106");
  specialFunctionsString.replace("DAC_1", "107");

  specialFunctionsString.replace("INA_N", "109");
  specialFunctionsString.replace("INA_P", "108");
  specialFunctionsString.replace("I_N", "109");
  specialFunctionsString.replace("I_P", "108");
  specialFunctionsString.replace("CURRENT_SENSE_MINUS", "109");
  specialFunctionsString.replace("CURRENT_SENSE_PLUS", "108");
  specialFunctionsString.replace("ISENSE_MINUS", "109");

  specialFunctionsString.replace("ISENSE_PLUS", "108");

  specialFunctionsString.replace("ISENSE_NEGATIVE", "109");
  specialFunctionsString.replace("ISENSE_POSITIVE", "108");
  specialFunctionsString.replace("ISENSE_POS", "108");
  specialFunctionsString.replace("ISENSE_NEG", "109");
  specialFunctionsString.replace("ISENSE_N", "109");
  specialFunctionsString.replace("ISENSE_P", "108");

  specialFunctionsString.replace("BUFFER_IN", "139");
  specialFunctionsString.replace("BUFFER_OUT", "140");
  specialFunctionsString.replace("BUF_IN", "139");
  specialFunctionsString.replace("BUF_OUT", "140");
  specialFunctionsString.replace("BUFF_IN", "139");
  specialFunctionsString.replace("BUFF_OUT", "140");
  specialFunctionsString.replace("BUFFIN", "139");
  specialFunctionsString.replace("BUFFOUT", "140");

  specialFunctionsString.replace("EMPTY_NET", "127");

  specialFunctionsString.replace("ADC0_8V", "110");
  specialFunctionsString.replace("ADC1_8V", "111");
  specialFunctionsString.replace("ADC2_8V", "112");
  specialFunctionsString.replace("ADC3_8V", "113");
  specialFunctionsString.replace("ADC4_5V", "114");
  specialFunctionsString.replace("PROBE_MEASURE", "115");
  specialFunctionsString.replace("115", "139");
  specialFunctionsString.replace("ADC0", "110");
  specialFunctionsString.replace("ADC1", "111");
  specialFunctionsString.replace("ADC2", "112");
  specialFunctionsString.replace("ADC3", "113");
  specialFunctionsString.replace("ADC4", "114");
  specialFunctionsString.replace("PROBE_MEASURE", "139");

  specialFunctionsString.replace("ADC_0", "110");
  specialFunctionsString.replace("ADC_1", "111");
  specialFunctionsString.replace("ADC_2", "112");
  specialFunctionsString.replace("ADC_3", "113");
  specialFunctionsString.replace("ADC_4", "114");
  specialFunctionsString.replace("ADC_7", "115");

  specialFunctionsString.replace("GPIO_1", "131");
  specialFunctionsString.replace("GPIO_2", "132");
  specialFunctionsString.replace("GPIO_3", "133");
  specialFunctionsString.replace("GPIO_4", "134");

  specialFunctionsString.replace("GPIO1", "131");
  specialFunctionsString.replace("GPIO2", "132");
  specialFunctionsString.replace("GPIO3", "133");
  specialFunctionsString.replace("GPIO4", "134");

  specialFunctionsString.replace("GPIO_5", "135");
  specialFunctionsString.replace("GPIO_6", "136");
  specialFunctionsString.replace("GPIO_7", "137");
  specialFunctionsString.replace("GPIO_8", "138");

  specialFunctionsString.replace("GPIO5", "135");
  specialFunctionsString.replace("GPIO6", "136");
  specialFunctionsString.replace("GPIO7", "137");
  specialFunctionsString.replace("GPIO8", "138");

  specialFunctionsString.replace("GP_1", "131");
  specialFunctionsString.replace("GP_2", "132");
  specialFunctionsString.replace("GP_3", "133");
  specialFunctionsString.replace("GP_4", "134");
  specialFunctionsString.replace("GP_5", "135");
  specialFunctionsString.replace("GP_6", "136");
  specialFunctionsString.replace("GP_7", "137");
  specialFunctionsString.replace("GP_8", "138");

  specialFunctionsString.replace("GP1", "131");
  specialFunctionsString.replace("GP2", "132");
  specialFunctionsString.replace("GP3", "133");
  specialFunctionsString.replace("GP4", "134");
  specialFunctionsString.replace("GP5", "135");
  specialFunctionsString.replace("GP6", "136");
  specialFunctionsString.replace("GP7", "137");
  specialFunctionsString.replace("GP8", "138");

  specialFunctionsString.replace("+5V", "105");
  specialFunctionsString.replace("5V", "105");
  specialFunctionsString.replace("3.3V", "103");
  specialFunctionsString.replace("3V3", "103");

  specialFunctionsString.replace("RP_UART_TX", "116");
  specialFunctionsString.replace("RP_UART_RX", "117");

  specialFunctionsString.replace("UART_TX", "116");
  specialFunctionsString.replace("UART_RX", "117");

  specialFunctionsString.replace("TX", "116");
  specialFunctionsString.replace("RX", "117");
}

void replaceNanoNamesWithDefinedInts(
    void) // for dome reason Arduino's String wasn't replacing like 1 or 2 of
// the names, so I'm using SafeString now and it works
{
  if (debugFP)
    Jerial.println("replacing special function names with defined ints\n\r");

  char nanoName[10];
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }

  itoa(NANO_D10, nanoName, 10);
  specialFunctionsString.replace("D10", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }

  itoa(NANO_D11, nanoName, 10);
  specialFunctionsString.replace("D11", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D12, nanoName, 10);
  specialFunctionsString.replace("D12", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D13, nanoName, 10);
  specialFunctionsString.replace("D13", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D0, nanoName, 10);
  specialFunctionsString.replace("D0", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D1, nanoName, 10);
  specialFunctionsString.replace("D1", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D2, nanoName, 10);
  specialFunctionsString.replace("D2", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D3, nanoName, 10);
  specialFunctionsString.replace("D3", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D4, nanoName, 10);
  specialFunctionsString.replace("D4", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D5, nanoName, 10);
  specialFunctionsString.replace("D5", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D6, nanoName, 10);
  specialFunctionsString.replace("D6", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D7, nanoName, 10);
  specialFunctionsString.replace("D7", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D8, nanoName, 10);
  specialFunctionsString.replace("D8", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_D9, nanoName, 10);
  specialFunctionsString.replace("D9", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_RESET, nanoName, 10);
  specialFunctionsString.replace("RESET", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_AREF, nanoName, 10);
  specialFunctionsString.replace("AREF", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A0, nanoName, 10);
  specialFunctionsString.replace("A0", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A1, nanoName, 10);
  specialFunctionsString.replace("A1", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A2, nanoName, 10);
  specialFunctionsString.replace("A2", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A3, nanoName, 10);
  specialFunctionsString.replace("A3", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A4, nanoName, 10);
  specialFunctionsString.replace("A4", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A5, nanoName, 10);
  specialFunctionsString.replace("A5", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A6, nanoName, 10);
  specialFunctionsString.replace("A6", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  itoa(NANO_A7, nanoName, 10);
  specialFunctionsString.replace("A7", nanoName);
  for (int i = 0; i < 10; i++) {
    nanoName[i] = ' ';
  }
  // if(debugFP)Jerial.println(bridgeString);
  if (debugFP)
    Jerial.println(specialFunctionsString);
  if (debugFP)
    Jerial.println("\n\n\r");
}

void parseStringToBridges(void) {

  // int bridgeStringLength = bridgeString.length() - 1;

  int specialFunctionsStringLength = specialFunctionsString.length() - 1;

  int readLength = 0;

  newBridgeLength = 0;
  newBridgeIndex = 0;

  if (debugFP) {
    Jerial.println("parsing bridges into array\n\r");
  }
  int stringIndex = 0;
  char delimitersCh[] = "[,- \n\r";

  createSafeString(buffer, 10);
  createSafeStringFromCharArray(delimiters, delimitersCh);
  int doneReading = 0;

  for (int i = 0; i <= specialFunctionsStringLength; i++) {

    stringIndex =
        specialFunctionsString.stoken(buffer, stringIndex, delimiters);
    if (stringIndex == -1) {
      break;
    }

    // Jerial.print("buffer = ");
    // Jerial.println(buffer);

    // Jerial.print("stringIndex = ");
    // Jerial.println(stringIndex);

    buffer.toInt(globalState.connections.paths[newBridgeIndex].node1);

    // Jerial.print("globalState.connections.paths[newBridgeIndex].node1 = ");
    // Jerial.println(globalState.connections.paths[newBridgeIndex].node1);

    if (debugFP) {
      Jerial.print("node1 = ");
      Jerial.println(globalState.connections.paths[newBridgeIndex].node1);
    }

    stringIndex =
        specialFunctionsString.stoken(buffer, stringIndex, delimiters);

    buffer.toInt(globalState.connections.paths[newBridgeIndex].node2);

    if (debugFP) {
      Jerial.print("node2 = ");
      Jerial.println(globalState.connections.paths[newBridgeIndex].node2);
    }

    readLength = stringIndex;

    if (readLength == -1) {
      doneReading = 1;
      break;
    }
    newBridgeLength++;
    newBridgeIndex++;

    if (debugFP) {
      Jerial.print("readLength = ");
      Jerial.println(readLength);
      Jerial.print("specialFunctionsString.length() = ");
      Jerial.println(specialFunctionsString.length());
    }

    if (debugFP)
      Jerial.print(newBridgeIndex);
    if (debugFP)
      Jerial.print("-");
    if (debugFP)
      Jerial.println(newBridge[newBridgeIndex][1]);
  }

  newBridgeIndex = 0;
  if (debugFP)
    for (int i = 0; i < newBridgeLength; i++) {
      Jerial.print("[");
      Jerial.print(globalState.connections.paths[newBridgeIndex].node1);
      Jerial.print("-");
      Jerial.print(globalState.connections.paths[newBridgeIndex].node2);
      Jerial.print("],");
      newBridgeIndex++;
    }
  if (debugFP)
    Jerial.print("\n\rbridge pairs = ");
  if (debugFP)
    Jerial.println(newBridgeLength);

  // nodeFileString.clear();

  // if(debugFP)Jerial.println(nodeFileString);
  timeToFP = millis() - timeToFP;
  if (debugFPtime)
    Jerial.print("\n\rtook ");

  if (debugFPtime)
    Jerial.print(timeToFP);
  if (debugFPtime)
    Jerial.println("ms to open and parse file\n\r");

  // printBridgeArray();

  // printNodeFile();
}

int lenHelper(int x) {
  if (x >= 1000000000)
    return 10;
  if (x >= 100000000)
    return 9;
  if (x >= 10000000)
    return 8;
  if (x >= 1000000)
    return 7;
  if (x >= 100000)
    return 6;
  if (x >= 10000)
    return 5;
  if (x >= 1000)
    return 4;
  if (x >= 100)
    return 3;
  if (x >= 10)
    return 2;
  return 1;
}

int printLen(int x) { return x < 0 ? lenHelper(-x) + 1 : lenHelper(x); }

// Helper functions for net color tracking and directory management
void ensureNetColorsDirectoryExists() {
  // if (!FatFS.exists("/net_colors")) {
  //   if (FatFS.mkdir("/net_colors")) {
  //     if (debugFP) {
  //       Jerial.println("Created /net_colors/ directory");
  //     }
  //   } else {
  //     if (debugFP) {
  //       Jerial.println("Failed to create /net_colors/ directory");
  //     }
  //   }
  // }
}

bool slotHasNetColors(int slot) {
  if (slot < 0 || slot >= 32) return false; // Support up to 32 slots with bitmask
  return (slotsWithNetColors & (1U << slot)) != 0;
}

void setSlotHasNetColors(int slot, bool hasColors) {
  if (slot < 0 || slot >= 32) return;
  if (hasColors) {
    slotsWithNetColors |= (1U << slot);
  } else {
    slotsWithNetColors &= ~(1U << slot);
  }
}

void removeNetColorFile(int slot) {
  String colorFileName = "/net_colors/netColorsSlot" + String(slot) + ".txt";
  if (safeFileExists(colorFileName.c_str())) {
    safeFileDelete(colorFileName.c_str());
    if (debugFP) {
      Jerial.println("Removed empty net color file: " + colorFileName);
    }
  }
  setSlotHasNetColors(slot, false);
}

bool slotIsValidated(int slot) {
  if (slot < 0 || slot >= 32) return false; // Support up to 32 slots with bitmask
  return (slotsValidated & (1U << slot)) != 0;
}

void setSlotValidated(int slot, bool validated) {
  if (slot < 0 || slot >= 32) return;
  if (validated) {
    slotsValidated |= (1U << slot);
  } else {
    slotsValidated &= ~(1U << slot);
  }
}

void markSlotAsModified(int slot) {
  // When a slot is modified, it needs re-validation
  setSlotValidated(slot, false);
  if (debugFP) {
    Jerial.println("Marked slot " + String(slot) + " as needing validation");
  }
}

void initializeNetColorTracking() {
  // Reset tracking variable
  slotsWithNetColors = 0;
  
  // Check if net colors directory exists
  if (!FatFS.exists("/net_colors")) {
    if (debugFP) {
      Jerial.println("Net colors directory does not exist. No existing colors to track.");
    }
    return;
  }
  
  // Scan for existing net color files using safe functions
  int foundFiles = 0;
  for (int slot = 0; slot < 32 && slot < NUM_SLOTS; slot++) {
    String colorFileName = "/net_colors/netColorsSlot" + String(slot) + ".txt";
    int32_t fileSize = safeFileSize(colorFileName.c_str(), 300);
    if (fileSize > 0) {
      setSlotHasNetColors(slot, true);
      foundFiles++;
      if (debugFP) {
        Jerial.println("Found net colors for slot " + String(slot));
      }
    }
  }
  
  if (debugFP) {
    Jerial.println("Initialized net color tracking. Found " + String(foundFiles) + " slots with colors.");
  }
}

void initializeValidationTracking() {
  // Reset validation tracking - all slots need validation on startup
  slotsValidated = 0;
  
  if (debugFP) {
    Jerial.println("Initialized validation tracking. All slots marked for validation on first use.");
  }
}





///@brief prints the disconnected nodes (separated by commas)
///@return the number of disconnected nodes
int printDisconnectedNodes() {
  int charCount = 0;

  if (lastRemovedNodesIndex > 0) {
    // Jerial.print("");
    for (int i = 0; i < lastRemovedNodesIndex; i++) {
      if (lastRemovedNodes[i] != -1) {
        charCount += printNodeOrName(lastRemovedNodes[i], 1);
        if (i < lastRemovedNodesIndex - 1 && i < 3) {
          Jerial.print(", ");
          charCount += 2;
        }
      }
    }
  }
  return lastRemovedNodesIndex;
}

///@brief returns the next disconnected node (repeated calls return the next
///one), returns -1 if no more disconnected nodes
///@return the next disconnected node, or -1 if no more disconnected nodes
int disconnectedNode() {
  static int lastIndex = 0;
  if (disconnectedNodeNewData) {
    lastIndex = 0;
    disconnectedNodeNewData = false;
  }
  if (lastIndex >= lastRemovedNodesIndex || lastRemovedNodesIndex == 0) {
    return -1;
  }
  return lastRemovedNodes[lastIndex++];
}

// DEPRECATED: This function is no longer used. Net colors are now persisted
// in the YAML state files (via States.cpp). The changedNetColors[] array
// is still used at runtime, but persistence is handled by the YAML system.
// This function remains for potential legacy file migration only.
int loadChangedNetColorsFromFile(int slot, int flashOrLocal) {
  // DEPRECATED: This function is no longer used. Net colors are now persisted
  // in the YAML state files (via States.cpp). This is now a no-op.
  return 0;
}

// DEPRECATED: Net colors are now persisted in YAML state files.
// This function remains for debugging/migration purposes only.
void printAllChangedNetColorFiles(void) {
  bool foundAnyColors = false;
  
  for (int i = 0; i < NUM_SLOTS; i++) {
    // Only check slots that have colors according to our tracking variable
    if (slotHasNetColors(i)) {
      foundAnyColors = true;
    //  Jerial.println("Slot " + String(i) + ":");
      printChangedNetColorFile(i, 0);
    }
  }
  
  if (!foundAnyColors) {
   // Jerial.println("No slots have net color overrides.");
  }
}

int printChangedNetColorFile(int slot, int flashOrLocal) {
  if (flashOrLocal == 1) {
    // Print from cache (currentColorSlotColorsString)
    // The cache always refers to the current netSlot, so 'slot' param is
    // implicitly current netSlot
    {
      unsigned long _t = micros();
      while (core2busy) {
        __dmb();
        // ponytail: timeout-and-proceed only; never falsify core 2's flag
        if (micros() - _t > 25000) break;
        TinyUSB_Device_Task(); // mutex-guarded pump
      }
    }
    core1busy = true;

    if (debugFP) {
      Jerial.println(
          "Printing cached net colors (currentColorSlotColorsString):");
    }
    if (currentColorSlotColorsString.isEmpty()) {
    //  Jerial.println("[Cache is empty or not yet loaded]");
    } else {
      Jerial.print(currentColorSlotColorsString);
      if (!currentColorSlotColorsString.endsWith(
              "\n")) { // Ensure a newline for cleaner output if not present
        Jerial.println();
      }
    }
    core1busy = false;
    return 1;
  }

  // flashOrLocal == 0, print from file
  // Fast check: if slot has no colors according to our tracking variable, skip file operations
  if (!slotHasNetColors(slot)) {
    // Jerial.println("Slot " + String(slot) + " has no net color overrides.");
    return 1; // Success - no colors is valid
  }

  String colorFileName = "/net_colors/netColorsSlot" + String(slot) + ".txt";

  if (!safeFileExists(colorFileName.c_str())) {
    Jerial.println("Color file " + colorFileName + " does not exist.");
    // File doesn't exist but tracking says it should - clear the tracking bit
    setSlotHasNetColors(slot, false);
    return 0;
  }

  if (::colorFile) {
    safeFileClose(::colorFile, false);
  }

  ::colorFile = safeFileOpen(colorFileName.c_str(), "r");

  if (!::colorFile) {
    if (debugFP) {
      Jerial.println("Failed to open " + colorFileName + " for printing.");
    }
    return 0;
  }

  if (debugFP) {
    Jerial.println("Printing contents of " + colorFileName + ":");
  }

  if (::colorFile.size() == 0) {
    Jerial.println("[File is empty]");
  } else {
    while (::colorFile.available()) {
      Jerial.write(::colorFile.read());
    }
    // Ensure a newline if the file doesn't end with one for cleaner console
    // output This requires peeking or reading the last char, which is slightly
    // complex here. Simpler: just print a newline if Jerial.print didn't just
    // do one (hard to track) For now, we assume file content includes newlines
    // or user handles formatting.
  }

  ::colorFile.close();
  return 1;
}

int saveChangedNetColorsToFile(int slot, int flashOrLocal) {
  // DEPRECATED: Net colors are now persisted via YAML state system (States.cpp)
  // This is now a no-op. Net colors are automatically saved with slot state.
  return 1; // Return success to maintain compatibility
}

// Node File Validation and Repair System
// =====================================
// This system prevents infinite loops and unresponsive behavior when bad data
// exists in node files (e.g., malformed connections like "ILES," without
// dashes).
//
// The repair process:
// 1. validateAndRepairNodeFile() - Main entry point, attempts repair up to
// maxRetries times
// 2. attemptNodeFileRepair() - Parses file, removes malformed connections,
// saves clean version
// 3. If repair fails completely, clears the file to ensure system remains
// responsive
//
// This is called from openNodeFile() before any parsing to prevent crashes
// during startup.

bool attemptNodeFileRepair(int slot) {
  if (debugFP) {
    Jerial.println("◇ Attempting to repair nodeFileSlot" + String(slot) +
                   ".txt");
  }

  String content = readSlotFileContent(slot);
  if (content.length() == 0) {
    return false; // Nothing to repair
  }

  // Extract content between braces
  int openBraceIdx = content.indexOf("{");
  int closeBraceIdx = content.indexOf("}");

  if (openBraceIdx == -1 || closeBraceIdx == -1) {
    if (debugFP) {
      Jerial.println("◇ Missing braces, creating clean file");
    }
    clearNodeFile(slot, 0);
    return true;
  }

  String connections = content.substring(openBraceIdx + 1, closeBraceIdx);
  connections.trim();

  if (connections.length() == 0) {
    return true; // Empty file is valid
  }

  // Split connections and validate each one
  String repairedConnections = "";
  int validConnections = 0;
  int startIdx = 0;
  int commaIdx = connections.indexOf(',', startIdx);

  while (startIdx < connections.length()) {
    String connection;

    if (commaIdx == -1) {
      connection = connections.substring(startIdx);
    } else {
      connection = connections.substring(startIdx, commaIdx);
      startIdx = commaIdx + 1;
      commaIdx = connections.indexOf(',', startIdx);
    }

    connection.trim();

    // Skip empty connections
    if (connection.length() == 0) {
      if (commaIdx == -1)
        break;
      continue;
    }

    // Validate connection format
    int dashIdx = connection.indexOf('-');
    if (dashIdx != -1) {
      String node1Str = connection.substring(0, dashIdx);
      String node2Str = connection.substring(dashIdx + 1);

      node1Str.trim();
      node2Str.trim();

      // Check if both parts are valid numbers or known names
      bool valid = true;
      int node1 = node1Str.toInt();
      int node2 = node2Str.toInt();

      // If toInt() returns 0, check if it was actually "0" or a failed
      // conversion
      if ((node1 == 0 && node1Str != "0") || (node2 == 0 && node2Str != "0")) {
        // Check if they're valid node names (will be converted later)
        if (node1Str.length() < 2 || node2Str.length() < 2) {
          valid = false;
        }
      } else {
        // Validate numeric node numbers
        if (isNodeValid(node1) != 1 || isNodeValid(node2) != 1) {
          valid = false;
        }
      }

      if (valid) {
        if (repairedConnections.length() > 0) {
          repairedConnections += ",";
        }
        repairedConnections += connection;
        validConnections++;
      } else {
        if (debugFP) {
          Jerial.println("◇ Removed invalid connection: '" + connection + "'");
        }
      }
    } else {
      if (debugFP) {
        Jerial.println("◇ Removed malformed connection (no dash): '" +
                       connection + "'");
      }
    }

    if (commaIdx == -1)
      break;
  }

  // Write the repaired content back to file
  File slotFile = safeFileOpen(("nodeFileSlot" + String(slot) + ".txt").c_str(), "w");
  if (slotFile) {
    const char* openBrace = "{ ";
    slotFile.write((const uint8_t*)openBrace, strlen(openBrace));
    if (repairedConnections.length() > 0) {
      slotFile.write((const uint8_t*)repairedConnections.c_str(), repairedConnections.length());
    }
    const char* closeBrace = " }";
    slotFile.write((const uint8_t*)closeBrace, strlen(closeBrace));
    safeFileClose(slotFile, true);

    if (debugFP) {
      Jerial.println("◆ Repaired nodeFileSlot" + String(slot) + ".txt with " +
                     String(validConnections) + " valid connections");
    }
  }

  return validConnections >=
         0; // Success even if no valid connections (empty is valid)
}

bool validateAndRepairNodeFile(int slot, int maxRetries) {
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    int validation_result = validateNodeFileSlot(slot, debugFP);

    if (validation_result == 0) {
      if (debugFP && attempt > 0) {
        Jerial.println("◆ NodeFile validation passed after repair");
      }
      return true; // Valid
    }

    if (debugFP) {
      Jerial.println(
          "◇ NodeFile validation failed (attempt " + String(attempt + 1) +
          "): " + String(getNodeFileValidationError(validation_result)));
    }

    // Attempt repair
    if (!attemptNodeFileRepair(slot)) {
      if (debugFP) {
        Jerial.println("◇ Repair attempt failed");
      }
      break;
    }
  }

  // If we get here, repair failed - clear the file
  if (debugFP) {
    Jerial.println("◇ All repair attempts failed, clearing nodeFileSlot" +
                   String(slot) + ".txt");
  }
  clearNodeFile(slot, 0);
  return true; // Cleared file is valid
}

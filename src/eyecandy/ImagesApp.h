/**
 * Images App - OLED Image Viewer
 * 
 * Header file for the OLED image viewer application
 */

#ifndef IMAGES_APP_H
#define IMAGES_APP_H

#include <Arduino.h>

/**
 * Launch the Images App - browse and display OLED images from /images folder
 * @param selectionMode If true, returns filename on short click instead of just viewing
 * @return Empty string in viewer mode, selected filename in selection mode
 */
String imagesApp(bool selectionMode = false);

/**
 * Interactive image selector for menu configuration (wrapper for imagesApp)
 * @return Selected image filename (empty string on cancel)
 */
String selectImageFromMenu(void);

/**
 * Load and display an OLED image from filesystem
 * @param filename The image filename (without path, assumes /images/ folder)
 * @return true if image loaded successfully, false otherwise
 */
bool loadAndDisplayImage(const char* filename);

/**
 * Wrapper for Apps menu - discards return value
 */
void imagesAppLauncher(void);

void printColorJogo(void);
void printColorJogoSmall(void);

#endif // IMAGES_APP_H


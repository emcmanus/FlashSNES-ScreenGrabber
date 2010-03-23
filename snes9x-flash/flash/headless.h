/*
 *  headless.h
 *  SNES9x_FlashSnes
 *
 *  Created by Ed McManus on 2/23/10.
 *  Copyright 2008. All rights reserved.
 *
 */


// Flash Callbacks
int Flash_setup( const char *filename );
int Flash_teardown();

void S9xPostRomInit();
void S9xSetupDefaultKeymap();
void S9xProcessEvents();
void S9xMixNewSamples( int numSamples );

void prepareDisplayBuffer (int, int, int);
void printDefaultDecision( char *outputPrefix );
void OutOfMemory (void);

// PNG Encoder
int writeScreenshot( char *outputPrefix, int iteration );
// static void png_user_error( png_struct *s, const char *c );
// static void png_user_warn( png_struct *s, const char *c );

// Test func
void putpixel(int x, int y);
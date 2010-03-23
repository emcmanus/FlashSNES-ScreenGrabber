/*
 *  flash.cpp
 *  SNES9x_FlashSnes
 *
 *  Created by Ed McManus on 12/26/08.
 *  Copyright 2008. All rights reserved.
 *
 */

#include "snes9x.h"
#include "memmap.h"
#include "debug.h"
#include "cpuexec.h"
#include "ppu.h"
#include "snapshot.h"
#include "apu.h"
#include "display.h"
#include "gfx.h"
#include "soundux.h"
#include "spc700.h"
#include "spc7110.h"
#include "controls.h"
#include "conffile.h"
#include "logger.h"

#include "headless.h"


/*
 * Flash buffers - screen and sound
 */
uint32 *FlashDisplayBuffer;
float *FlashSoundBuffer;

int bufferedSamples;  // equal to bytes available


static const int NUMBER_CAPTURES = 20;

// Deltas: used for picking default thumbnail
static uint repeatPixelCounts[ NUMBER_CAPTURES ];    // Average pixel distance from the same previously displayed


void trace( const char *message )
{
  // fprintf( stdout, "[TRACE] %s\n", message );
}

void printUsage()
{
  fprintf( stderr, "\nUsage: ./program rom_file output_prefix\n(Output_prefix can be a relative or global path; typically includes a sub-directory.)\n\n" );
}


/*
 * CLib Setup
 */
   
int main (int argc, char **argv)
{	
	// CLI Args
	////////////////////////////////////////////////
  if ( argc != 3 )
  {
    printUsage();
    abort();
  }
  
  char *filename = argv[1];
  char *outputPrefix = argv[2];
  
  
	// Setup Functions
	////////////////////////////////////////////////
  if (Flash_setup( filename ))
  {
    fprintf( stderr, "Setup failed." );
    fprintf( stdout, "Setup failed." );
    printUsage();
    abort();
  }
  
	
	// mute
	Settings.APUEnabled = Settings.NextAPUEnabled = false;
  Settings.SoundSkipMethod = 1;
	
	
	// Screen cap stage
	////////////////////////////////////////////////
  int i, j;
  int initialIterations = 750;  // Skip first 15 seconds
  int iterations = 75;          // Cap every 1.5 seconds
  int caps = NUMBER_CAPTURES;   // Num frames to save
  
  for ( i=0; i < initialIterations; i++ )
  {
    S9xMainLoop();
  }
  
  for ( i=0; i<caps; i++ )
  {
    for ( j=0; j < iterations; j++ )
    {
      S9xMainLoop();
    }
    
    // Prepare buffer and get cap
    prepareDisplayBuffer(IMAGE_WIDTH, IMAGE_HEIGHT, i);
    
    if(writeScreenshot( outputPrefix, i ))
    {
      trace("Error writing screenshot.");
    }
  }
  
  
  // Exit
	////////////////////////////////////////////////
	printDefaultDecision( outputPrefix );
  Flash_teardown();
}


// Flush Display Buffer to PNG
#include "png.h"
int writeScreenshot( char *outputPrefix, int iteration )
{
  int width = 256;
  int height = 224;
  
  // Build destination filename
  if ( iteration > 99999 )
    return (1);
  
  char destFilename[300];
  sprintf(destFilename, "%.*s%.5i.png", sizeof(destFilename)-9, outputPrefix, iteration); // 5 chars for iteration number, 4 for extension
  
  FILE *fp = fopen(destFilename, "wb");
  
  if (!fp)
  {
    return (1);
  }
  
  png_structp png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  
  if (!png_ptr)
  {
    return (1);
  }
  
  png_infop info_ptr = png_create_info_struct(png_ptr);
  
  if (!info_ptr)
  {
    png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
    return (1);
  }
  
  // Image parameters
  int bit_depth = 8;
  
  png_set_IHDR( png_ptr, info_ptr, width, height, bit_depth, PNG_COLOR_TYPE_RGB_ALPHA, 
                    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  
  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return (1);
  }
  
  png_init_io(png_ptr, fp);
  
  // Get row pointers
  png_byte *row_pointers[height];
  
  int y;
  for ( y=0; y<height; y++ )
  {
    row_pointers[y] = (png_byte *)(FlashDisplayBuffer + y * (GFX.Pitch>>1));
    // row_pointers[y] = (png_byte *)(GFX.Screen + y * (GFX.Pitch>>1));
  }
  
  // Set image data
  png_set_rows( png_ptr, info_ptr, row_pointers );
  
  // Write
  png_write_png( png_ptr, info_ptr, PNG_TRANSFORM_BGR | PNG_TRANSFORM_SWAP_ALPHA | PNG_TRANSFORM_INVERT_ALPHA, NULL );
  
  // Cleanup
  png_destroy_write_struct( &png_ptr, &info_ptr );
  
  fclose( fp );
  
  return( 0 );
}


int Flash_setup( const char *filename )
{  
	// Clear the settings struct
	ZeroMemory (&Settings, sizeof (Settings));
	
	// General
	Settings.ShutdownMaster = true; // Optimization -- Disable if this appears to cause any compatability issues -- it's known to for some games
	Settings.BlockInvalidVRAMAccess = true;
	Settings.HDMATimingHack = 100;
	Settings.Transparency = true;
	Settings.SupportHiRes = false;
	Settings.SDD1Pack = true;
	
	// Sound
	Settings.SoundPlaybackRate = 44100;
	Settings.Stereo = true;
	Settings.SixteenBitSound = true;
  Settings.DisableSoundEcho = true;
	Settings.SoundEnvelopeHeightReading = true;
	Settings.DisableSampleCaching = true;
	Settings.InterpolatedSound = true;
	
	// Controllers
	Settings.JoystickEnabled = false;
	Settings.MouseMaster = false;
	Settings.SuperScopeMaster = false;
	Settings.MultiPlayer5Master = false;
	Settings.JustifierMaster = false;
  
  // old settings
  Settings.APUEnabled = Settings.NextAPUEnabled = true;
  
  Settings.Multi = false;
	Settings.StopEmulation = true;
	
	// So listen, snes9x, we don't have any controllers. That's OK, yeah?
	S9xReportControllers();
	
	// Initialize system memory
	if (!Memory.Init() || !S9xInitAPU())	// We'll add sound init here later!
		OutOfMemory ();
	
	Memory.PostRomInitFunc = S9xPostRomInit;
	
	// Further sound initialization
	S9xInitSound(7, Settings.Stereo, Settings.SoundBufferSize); // The 7 is our "mode" and isn't really explained anywhere. 7 ensures that OpenSoundDevice is called.
	
	uint32 saved_flags = CPU.Flags;
	
	// Load test ROM
	bool8 loaded;
	
	// TODO - get this from the CLI
	loaded = Memory.LoadROM( filename );
	
	Settings.StopEmulation = !loaded;
	
	if (!loaded) {
    trace("Error Loading ROM file.");
		return 1;
	}
	
	CPU.Flags = saved_flags;
	
	// Initialize GFX members
	GFX.Pitch = IMAGE_WIDTH * 2;
	GFX.Screen = (uint16 *) malloc (GFX.Pitch * IMAGE_HEIGHT);
	GFX.SubScreen = (uint16 *) malloc (GFX.Pitch * IMAGE_HEIGHT);
	GFX.ZBuffer = (uint8 *) malloc ((GFX.Pitch >> 1) * IMAGE_HEIGHT);
	GFX.SubZBuffer = (uint8 *) malloc ((GFX.Pitch >> 1) * IMAGE_HEIGHT);
	
	if (!GFX.Screen || !GFX.SubScreen)
	    OutOfMemory();
	
	// Initialize 32-bit version of GFX.Screen
	FlashDisplayBuffer = (uint32 *) malloc ((GFX.Pitch << 1) * IMAGE_HEIGHT);
	
	ZeroMemory (FlashDisplayBuffer, (GFX.Pitch << 1) * IMAGE_HEIGHT);
	ZeroMemory (GFX.Screen, GFX.Pitch * IMAGE_HEIGHT);
	ZeroMemory (GFX.SubScreen, GFX.Pitch * IMAGE_HEIGHT);
	ZeroMemory (GFX.ZBuffer, (GFX.Pitch>>1) * IMAGE_HEIGHT);
	ZeroMemory (GFX.SubZBuffer, (GFX.Pitch>>1) * IMAGE_HEIGHT);
	
	if (!S9xGraphicsInit())
		OutOfMemory();
	
	S9xSetupDefaultKeymap();
	
	return 0;
}



int Flash_teardown ()
{
	Memory.Deinit();
  return 0;
}



/*
 * Keyboard Input
 */

void S9xProcessEvents()
{
  // S9xReportButton( scanCode, keyState );
}


void S9xSetupDefaultKeymap()
{	
	S9xUnmapAllControls();
	
	// Build key map
	s9xcommand_t cmd;
	
	S9xMapButton( 65, cmd = S9xGetCommandT("Joypad1 Left"), false );    // A
	S9xMapButton( 68, cmd = S9xGetCommandT("Joypad1 Right"), false );   // D
	S9xMapButton( 87, cmd = S9xGetCommandT("Joypad1 Up"), false );      // W
	S9xMapButton( 83, cmd = S9xGetCommandT("Joypad1 Down"), false );    // S
	
	S9xMapButton( 79, cmd = S9xGetCommandT("Joypad1 X"), false );       // O
	S9xMapButton( 80, cmd = S9xGetCommandT("Joypad1 Y"), false );       // P
	S9xMapButton( 75, cmd = S9xGetCommandT("Joypad1 A"), false );       // K
  S9xMapButton( 76, cmd = S9xGetCommandT("Joypad1 B"), false );       // L
  
  S9xMapButton( 88, cmd = S9xGetCommandT("Joypad1 L"), false );       // X
  S9xMapButton( 77, cmd = S9xGetCommandT("Joypad1 R"), false );       // M
  
  S9xMapButton( 13, cmd = S9xGetCommandT("Joypad1 Start"), false );   // Enter
  S9xMapButton( 16, cmd = S9xGetCommandT("Joypad1 Select"), false );  // Shift
}



/*
 * Required interface methods specified in Porting.html
 */

void S9xParseArg (char **argv, int &i, int argc)
{  
}

bool8 S9xOpenSnapshotFile (const char *filepath, bool8 read_only, STREAM *file)
{
	return false;
}

void S9xExit (void)
{
}

bool S9xPollButton(uint32 id, bool *pressed){
    return false;
}

bool S9xPollAxis(uint32 id, int16 *value){
    return false;
}

bool S9xPollPointer(uint32 id, int16 *x, int16 *y){
    return false;
}

void S9xHandlePortCommand(s9xcommand_t cmd, int16 data1, int16 data2)
{
}

void S9xClosesnapshotFile (STREAM file)
{
}

bool8 S9xContinueUpdate(int width, int height)
{
	return (TRUE);
}

const char *S9xStringInput(const char *message)
{
  return "";
}

void S9xExtraUsage()
{
}

void S9xParsePortConfig(ConfigFile &conf, int pass){
}

bool8 S9xInitUpdate (void) // Screen is *about* to be rendered
{
	return true;
}

bool8 S9xDeinitUpdate (int width, int height) // Screen has been rendered
{
  if (!Settings.StopEmulation)
  {
    // Convert16To24(IMAGE_WIDTH, IMAGE_HEIGHT);
  }
  return true;
}

void S9xMessage (int type, int number, const char *message)
{
    // printf( "%s", message );
}

const char *S9xGetFilename (const char *extension, enum s9x_getdirtype dirtype)
{
  static char filename [PATH_MAX + 1];
  char dir [_MAX_DIR + 1];
  char drive [_MAX_DRIVE + 1];
  char fname [_MAX_FNAME + 1];
  char ext [_MAX_EXT + 1];
  _splitpath (Memory.ROMFilename, drive, dir, fname, ext);
  snprintf(filename, sizeof(filename), "%s" SLASH_STR "%s%s", S9xGetDirectory(dirtype), fname, extension);
		
  return (filename);
}

const char *S9xGetFilenameInc (const char *extension, enum s9x_getdirtype dirtype)
{
	return "";
}

const char *S9xGetDirectory (enum s9x_getdirtype dirtype)
{
	return ".";
}

START_EXTERN_C
const char* osd_GetPackDir()
{
	return "";
}
END_EXTERN_C

const char *S9xChooseFilename (bool8 read_only)
{
	return "";
}

const char *S9xChooseMovieFilename (bool8 read_only)
{
	return "";
}

const char *S9xBasename (const char *path)
{
	return "";
}

void S9xAutoSaveSRAM (void)
{
}


/*
  Sound API Start
*/

bool8 S9xOpenSoundDevice (int mode, bool8 stereo, int buffer_size)
{
	return true;
}
void S9xMixNewSamples( int numSamples )
{
}

void S9xGenerateSound (void)
{
}

void S9xToggleSoundChannel (int c)
{
}



void S9xSetPalette (void)
{
}

void S9xSyncSpeed (void)
{
		IPPU.RenderThisFrame = true;
		IPPU.FrameSkip = 0;
		IPPU.SkippedFrames = 0;
}

void S9xLoadSDD1Data (void)
{
}


// These methods are part of the driver interface although aren't mentioned in porting.html
void _makepath (char *path, const char *, const char *dir, const char *fname, const char *ext)
{
    if (dir && *dir)
    {
    	strcpy (path, dir);
    	strcat (path, "/");
    }
    else
	    *path = 0;
	  
    strcat (path, fname);
    if (ext && *ext)
    {
        strcat (path, ".");
        strcat (path, ext);
    }
}

void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext)
{
  *drive = 0;

  char *slash = strrchr(path, SLASH_CHAR);
  char *dot = strrchr(path, '.');

  if (dot && slash && dot < slash) {
    dot = 0;
  }

  if (!slash) {
    *dir = 0;
    strcpy(fname, path);
    if (dot) {
      fname[dot - path] = 0;
      strcpy(ext, dot + 1);
    } else {
      *ext = 0;
    }
  } else {
    strcpy(dir, path);
    dir[slash - path] = 0;
    strcpy(fname, slash + 1);
    if (dot) {
      fname[(dot - slash) - 1] = 0;
      strcpy(ext, dot + 1);
    } else {
      *ext = 0;
    }
  }
}


/*
 * Other methods
 */
void OutOfMemory (void) {
    S9xTracef( "STDERR: Snes9X: Memory allocation failure -"
             " not enough RAM/virtual memory available.\n"
             "S9xExiting...\n");
    Memory.Deinit ();

    exit (1);
}


// Default Heuristic - select the frame before the highest repeatedPixelCount

void printDefaultDecision( char *outputPrefix )
{
  uint i, highestRepeatIndex, highestRepeatCount;
  
  highestRepeatIndex = highestRepeatCount = 0;
  
  for (i=0; i<(uint)NUMBER_CAPTURES; i++)
  {
    if ( repeatPixelCounts[i] > highestRepeatCount )
    {
      highestRepeatIndex = i;
      highestRepeatCount = repeatPixelCounts[i];
    }
  }
  
  if (highestRepeatIndex > 0)
  {
    fprintf( stdout, "\n%s%.5i.png\n", outputPrefix, (highestRepeatIndex - 1) );
  }
}


// Color Depth Upcast: RGB565 -> RGBA8888
void prepareDisplayBuffer (int width, int height, int captureNumber)
{
  uint repeatPixels = 0;            // repeatPixels => number of pixels unchanged from last frame
  
	for (register int y = 0; y < height; y++)     // For each row
	{
	    register uint32 *d = (uint32 *) (FlashDisplayBuffer + y * (GFX.Pitch>>1));		// destination ptr
	    register uint16 *s = (uint16 *) (GFX.Screen + y * (GFX.Pitch>>1));				    // source ptr
	    
	    for (register int x = 0; x < width; x++)  // For each column
	    {
	      uint16 pixel = *s++;
	      
        uint32 oldValue = *d;
        uint32 newValue = (((pixel >> 11) & 0x1F) << (24 + 3)) |       // left-most 5 bits from source, shifted into MSB position
  				   (((pixel >> 5) & 0x3F) << (16 + 2)) |        // middle 6 bits
             ((pixel & 0x1f) << (8 + 3));                 // right-most 5 bits
  			
        *d = newValue;
  			
  			*d++;
  			
        if ( oldValue == newValue && oldValue > 0 )
        {
          repeatPixels++;
        }
	    }
	}
	// Store dela
  repeatPixelCounts[ captureNumber ] = repeatPixels;
}

void putpixel(int x, int y) {
	int bpp = 2;
	
	uint8 *p = (uint8 *)GFX.Screen + y * GFX.Pitch + x * bpp;
	*(uint16 *)p = 0x1F;
}


// All printf statements are re-routed here

#include <stdarg.h>
void S9xTracef( const char *format, ... )
{
  printf( "[TRACE]: %s\n", format );
}


void S9xTraceInt( int val )
{
}


void S9xPostRomInit()
{  
  if (!strncmp((const char *)Memory.NSRTHeader+24, "NSRT", 4))
  {
    //First plug in both, they'll change later as needed
    S9xSetController(0, CTL_JOYPAD,     0, 0, 0, 0);
    S9xSetController(1, CTL_JOYPAD,     1, 0, 0, 0);

    switch (Memory.NSRTHeader[29])
    {
      case 0: //Everything goes
       break;

      case 0x10: //Mouse in Port 0
        S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
        break;

      case 0x01: //Mouse in Port 1
        S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
        break;

      case 0x03: //Super Scope in Port 1
        S9xSetController(1, CTL_SUPERSCOPE, 0, 0, 0, 0);
        break;

      case 0x06: //Multitap in Port 1
        S9xSetController(1, CTL_MP5,        1, 2, 3, 4);
        break;

      case 0x66: //Multitap in Ports 0 and 1
        S9xSetController(0, CTL_MP5,        0, 1, 2, 3);
        S9xSetController(1, CTL_MP5,        4, 5, 6, 7);
        break;

      case 0x08: //Multitap in Port 1, Mouse in new Port 1
        S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
        //There should be a toggle here for putting in Multitap instead
        break;

      case 0x04: //Pad or Super Scope in Port 1
        S9xSetController(1, CTL_SUPERSCOPE, 0, 0, 0, 0);
        //There should be a toggle here for putting in a pad instead
        break;

      case 0x05: //Justifier - Must ask user...
        S9xSetController(1, CTL_JUSTIFIER,  1, 0, 0, 0);
        //There should be a toggle here for how many justifiers
        break;

      case 0x20: //Pad or Mouse in Port 0
        S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
        //There should be a toggle here for putting in a pad instead
        break;

      case 0x22: //Pad or Mouse in Port 0 & 1
        S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
        S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
        //There should be a toggles here for putting in pads instead
        break;

      case 0x24: //Pad or Mouse in Port 0, Pad or Super Scope in Port 1
        //There should be a toggles here for what to put in, I'm leaving it at gamepad for now
        break;

      case 0x27: //Pad or Mouse in Port 0, Pad or Mouse or Super Scope in Port 1
        //There should be a toggles here for what to put in, I'm leaving it at gamepad for now
        break;

      //Not Supported yet
      case 0x99: break; //Lasabirdie
      case 0x0A: break; //Barcode Battler
    }
  }
}





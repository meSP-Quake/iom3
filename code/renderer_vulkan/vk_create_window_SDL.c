/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/


// the window surface needs to be createdd right after the instance creation
// because it can actually influence the physical device selection

#include "VKimpl.h"
#include "vk_instance.h"

#include "tr_cvar.h"
#include "icon_oa.h"
#include "glConfig.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>


#ifdef USE_INTERNAL_SDL_HEADERS
#   include "SDL.h"
#   include "SDL_vulkan.h"
#else
#	include <SDL3/SDL.h>
#   include <SDL3/SDL_vulkan.h>
#endif


static SDL_Window* window_sdl = NULL;
static SDL_PropertiesID properties = 0;

static cvar_t* r_displayIndex;

/*
====================================
VKimp_GetCurrentDisplayID
====================================
Gets current display window is drawn at or last known display
that window has been drawed at ( r_displayIndex ).

Fails only if it's impossible to get information about displays.
*/
static SDL_DisplayID VKimp_GetCurrentDisplayID(void) {
	int numDisplays;
	SDL_DisplayID *displays = NULL;
	SDL_DisplayID result = 0;

	if (window_sdl) {
		result = SDL_GetDisplayForWindow(window_sdl);

		// Window could be hidden and underlying display could not be retrieved
		// by the window manager.
		if (result != 0) {
			return result;
		}
	}

	// So, we don't have a window or it's hidden, so try to get the last known
	// display the window has been drawed at.
	// r_displayIndex should always stay updated. If it's not, fix that first, and
	// only then go here.

	// We would index through that with r_displayIndex so same index corresponds
	// to the same display, even if DisplayID is always unique.
	displays = SDL_GetDisplays(&numDisplays);

	if (displays == NULL) {
		ri.Printf(PRINT_ERROR, "VKimp_GetCurrentDisplayID() failed to retrieve displays: %s\n", SDL_GetError());
		return result;
	}

	if (numDisplays <= 0) {
		// Should never happend, but possibly system tries to hide display configurations?
		result = 0;

		ri.Printf(PRINT_ERROR, "VKimp_GetCurrentDisplayID() sees no display: %s\n", SDL_GetError());
	} else if (r_displayIndex->integer < 0 || r_displayIndex->integer >= numDisplays) {
		ri.Printf(
			PRINT_WARNING,
			"VKimp_GetCurrentDisplayID() attempted to access invalid display %i. Only displays from 0 to %i are available.\n",
			r_displayIndex->integer,
			numDisplays - 1
		);
	} else {
		result = displays[r_displayIndex->integer];
	}

	SDL_free(displays);

	return result;
}

/*
====================================
VKimp_RetrieveAvailableModes
====================================
Lists available fullscreen modes for current display. Returns pointers to
display modes which can be passed as valid arguments to SDL functions.

Returned value should be freed if you don't use it.
*/
static SDL_DisplayMode** VKimp_RetrieveAvailableModes(int* numAvailableModes) {
	static SDL_DisplayMode **modes = NULL;

	int numModes;
	int i, j;
	SDL_DisplayMode **availableModes;
	int numFilteredModes;
	SDL_DisplayID display = VKimp_GetCurrentDisplayID();

	if (modes) {
		SDL_free(modes);
	}

	if (!display) {
		return NULL;
	}

	modes = SDL_GetFullscreenDisplayModes( display, &numModes );

	if (!modes || numModes <= 0) {
		ri.Printf(PRINT_WARNING, "VKimp_RetrieveAvailableModes() failed to retrieve fullscreen display modes: %s\n", SDL_GetError());
		return NULL;
	}

	availableModes = SDL_calloc(numModes, sizeof( SDL_DisplayMode* ));

	if (!availableModes) {
		ri.Printf(PRINT_WARNING, "VKimp_RetrieveAvailableModes(): failed to allocate %lu bytes of memory.\n", numModes * sizeof( SDL_DisplayMode* ));

		numModes = 0;
	}

	numFilteredModes = 0;

	for (i = 0; i < numModes; ++i) {
		SDL_DisplayMode *mode;

		if (!modes[i]) continue;

		mode = modes[i];

		for (j = 0; j < numFilteredModes; ++j) {
			if (availableModes[j]->w == mode->w && availableModes[j]->h == mode->h) {
				break;
			}
		}

		if (j < numFilteredModes) {
			continue;  // Found duplicate mode
		}

		availableModes[j] = mode;
		numFilteredModes++;
	}

	if (numAvailableModes) {
		*numAvailableModes = numFilteredModes;
	}
	
	return availableModes;
}

/*
====================================
VKimp_ListModes
====================================
Print all modes current display supports ( not predefined ones ).
*/
static void VKimp_ListModes(SDL_DisplayMode **modes, int numModes) {
	int i;

	for (i = 0; i < numModes; ++i) {
		ri.Printf(PRINT_ALL, "Mode %2i: %ix%i@%.2f\n", i, modes[i]->w, modes[i]->h, modes[i]->refresh_rate);
	}
}

void R_ModeList_f() {
	int numModes;
	SDL_DisplayMode **availableModes = VKimp_RetrieveAvailableModes(&numModes);

	ri.Printf(PRINT_ALL, "\n");

	VKimp_ListModes(availableModes, numModes);

	ri.Printf(PRINT_ALL, "\n");
}

/*
====================================
VKimp_GetDisplayMode
====================================
Convert numeric mode into SDL_DisplayMode that can be used in SDL calls.
Handles special modes like -2 and -1.
*/
static const SDL_DisplayMode *VKimp_GetDisplayMode(int mode) {
	static SDL_DisplayMode fallbackMode;

	int numModes = 0;
	SDL_DisplayMode **availableModes;
	const SDL_DisplayMode *displayMode;
	const SDL_DisplayMode *currentMode = &fallbackMode;

	availableModes = VKimp_RetrieveAvailableModes(&numModes);

	if (!availableModes || numModes <= 0) {
		ri.Printf(PRINT_WARNING, "VKimp_GetResolution() failed to retrieve available modes.\n");

		availableModes = NULL;
		numModes = 0;
	}

	if (mode < -2 || mode >= numModes) {
		if (numModes > 0) {
			mode = 0;

			ri.Printf(
				PRINT_WARNING,
				"VKimp_GetResolution(): mode %i is outside of acceptable range -2..%i. Switching to mode 0 ( %ix%i )\n",
				mode, numModes - 1,
				availableModes[0]->w, availableModes[0]->h
			);

			ri.Printf(PRINT_ALL, "Available modes:\n");
			VKimp_ListModes(availableModes, numModes);
		} else {
			mode = -1;
		}
	}

	if (mode == -2) {
		displayMode = SDL_GetCurrentDisplayMode( VKimp_GetCurrentDisplayID() );

		if (displayMode) {
			currentMode = displayMode;

			ri.Printf(
				PRINT_ALL,
				"VKimp_GetResolution(): initialized mode -2, %ix%i@%.2fhz\n",
				displayMode->w, displayMode->h, displayMode->refresh_rate
			);
		} else {
			mode = 0;

			ri.Printf(PRINT_WARNING, "VKimp_GetResolution(): failed to retrieve native resolution with r_mode -2. Defauling to mode 0.\n");
		}
	}

	if (mode >= 0 && numModes > 0 && availableModes) {
		currentMode = availableModes[mode];
	} else {
		fallbackMode.w = 0;
		fallbackMode.h = 0;
		fallbackMode.refresh_rate = 60.0f;

		// We're not using mode provided by SDL, so mark it so
		fallbackMode.internal = NULL;

		R_GetWinResolution(&fallbackMode.w, &fallbackMode.h);

		// Setting resolution to 0x0 can lead to crashes
		if (fallbackMode.w <= 0 || fallbackMode.h <= 0) {
			fallbackMode.w = 640;
			fallbackMode.h = 480;
		}
	}

	SDL_free(availableModes);

	return currentMode;
}

/*
====================================
VKimp_CreateWindow
====================================
Creates new window ( regardless of window_sdl ).
`display` can be provided to force window spawning at specific display.
`mode` can be hand-written value, not strictly something returned by SDL.
`propsOut` is an address where props shall be stored. When destroying window,
props should be destroyed as well.
*/
static SDL_Window* VKimp_CreateWindow(SDL_DisplayID display, const SDL_DisplayMode* mode, qboolean fullscreen, SDL_PropertiesID *propsOut) {
	SDL_Window* window;
	SDL_PropertiesID props;
	SDL_WindowFlags flags = SDL_WINDOW_VULKAN;

	props = SDL_CreateProperties();

	if (fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN;
		flags |= SDL_WINDOW_BORDERLESS;
	}

	if (!props) {
		ri.Printf(PRINT_WARNING, "VKimp_CreateWindow() failed to create window properties: %s\n", SDL_GetError());

		return SDL_CreateWindow(
			CLIENT_WINDOW_TITLE,
			mode->w, mode->h,
			flags
		);
	}

	if (display) {
		SDL_Rect rect;

		SDL_GetDisplayBounds(display, &rect);

		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	}

	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, CLIENT_WINDOW_TITLE);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, qfalse);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, mode->w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, mode->h);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, qtrue);

	if (fullscreen) {
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, qtrue);
	}

	window = SDL_CreateWindowWithProperties(props);

	if (!window) {
		SDL_DestroyProperties(props);

		return NULL;
	}

	*propsOut = props;

	if (fullscreen && mode->internal) {
		if (!SDL_SetWindowFullscreenMode(window, mode)) {
			ri.Printf(PRINT_ERROR, "VKimp_CreateWindow() failed to set fullscreen mode: %s\n", SDL_GetError());
		} else {
			SDL_SetWindowFullscreen(window, qtrue);
			ri.Printf(PRINT_ALL, "VKimp_CreateWindow() initialized fullscreen mode.\n");
		}
	}

	SDL_SyncWindow(window);

	return window;
}

/*
====================================
VKimp_GetDisplayIndex
====================================
Restores display index by its ID.
*/
static int VKimp_GetDisplayIndex(SDL_DisplayID display) {
	int numDisplays;
	int i;
	SDL_DisplayID *displays;

	if (!display) return 0;

	displays = SDL_GetDisplays(&numDisplays);

	if (numDisplays <= 0 || !displays) {
		return -1;
	}

	for (i = 0; i < numDisplays; i++) {
		if (display == displays[i]) {
			return i;
		}
	}

	SDL_free(displays);

	return -1;
}

static int VKimp_SetMode(int mode, qboolean fullscreen) {
	const SDL_DisplayMode* currentMode;
	SDL_DisplayID currentDisplay = VKimp_GetCurrentDisplayID();

	if (window_sdl) {
		SDL_DestroyWindow(window_sdl);
		if (properties) SDL_DestroyProperties(properties);

		window_sdl = NULL;
		properties = 0;
	}

	R_SetWinMode(-1, 640, 480, 60);

	currentMode = VKimp_GetDisplayMode(mode);

	R_SetWinMode(fullscreen ? -2 : -3, currentMode->w, currentMode->h, currentMode->refresh_rate);

	window_sdl = VKimp_CreateWindow(currentDisplay, currentMode, fullscreen, &properties);

	if (!window_sdl) {
		return -1;
	}

	// Update r_displayIndex. OS can still force spawn window at different display, even
	// if we told the desired one.
	currentDisplay = SDL_GetDisplayForWindow(window_sdl);

	if (currentDisplay) {
		r_displayIndex->integer = VKimp_GetDisplayIndex(currentDisplay);
		ri.Cvar_SetValue(r_displayIndex->name, r_displayIndex->integer);
	}

	ri.Printf(PRINT_ALL, "VKimp_SetMode(): initialized window at %ix%i@%.2fhz on display %i.\n", currentMode->w, currentMode->h, currentMode->refresh_rate, r_displayIndex->integer);

	return 0;
}


/*
 * This routine is responsible for initializing the OS specific portions of Vulkan
 */
void vk_createWindow(void)
{
	ri.Printf(PRINT_ALL, "\n...Creating window (using SDL3)...\n");

	r_displayIndex = ri.Cvar_Get( "r_displayIndex", "-1", CVAR_ARCHIVE | CVAR_LATCH );

	SDL_Surface* icon = SDL_CreateSurfaceFrom(
			CLIENT_WINDOW_ICON.width,
			CLIENT_WINDOW_ICON.height,
			SDL_PIXELFORMAT_XRGB8888,
			(void *)CLIENT_WINDOW_ICON.pixel_data,
			CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width
			);

	if (icon == NULL)
	{
		ri.Printf(PRINT_ERROR, " SDL_CreateRGBSurface Failed. \n" );
	}

	if (ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_fullscreen", "0" );
        ri.Cvar_Set( "r_mode", "3" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	// Use this function to get a mask of the specified
	// subsystems which have previously been initialized.
	// If flags is 0 it returns a mask of all initialized subsystems,
	// otherwise it returns the initialization status of the specified subsystems.
	if (0 == SDL_WasInit(SDL_INIT_VIDEO))
	{
		ri.Printf(PRINT_ALL, " Video is not initialized before, so initial it.\n");

		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			ri.Printf(PRINT_ALL, " SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
		}
		else
		{
			ri.Printf(PRINT_ALL, " SDL using driver \"%s\"\n", SDL_GetCurrentVideoDriver( ));
		}
	}
	else
	{
		ri.Printf(PRINT_ALL, " Video is already initialized.\n");
	}

	if( 0 == VKimp_SetMode(r_mode->integer, r_fullscreen->integer) )
	{
		goto success;
	}
	else
	{
		ri.Printf(PRINT_ALL, " Setting r_mode=%d, r_fullscreen=%d failed, falling back on r_mode=%d\n",
				r_mode->integer, r_fullscreen->integer, 3 );

		if( 0 == VKimp_SetMode(3, qfalse) )
		{
			goto success;
		}
		else
		{
			ri.Error(ERR_FATAL, "VKimp_Init() - could not load Vulkan subsystem: %s", SDL_GetError());
		}
	}


success:

	SDL_SetWindowIcon( window_sdl, icon );

	SDL_DestroySurface( icon );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init(window_sdl);
}


void vk_getInstanceProcAddrImpl(void)
{
	ri.Printf(PRINT_ALL, " *** Vulkan Initialization ***\n");

    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        ri.Error(ERR_FATAL, "Failed to load Vulkan library: %s", SDL_GetError());
    }
    // Create the window 

    qvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) SDL_Vulkan_GetVkGetInstanceProcAddr();
    if( qvkGetInstanceProcAddr == NULL)
    {
        ri.Error(ERR_FATAL, "Failed to find entrypoint vkGetInstanceProcAddr\n"); 
    }
    
    ri.Printf(PRINT_ALL,  " Get instance proc address. (using SDL3)\n");
}


void vk_destroyWindow( void )
{
	SDL_DisplayID currentDisplay = SDL_GetDisplayForWindow(window_sdl);

	if (currentDisplay) {
		r_displayIndex->integer = VKimp_GetDisplayIndex(currentDisplay);
		ri.Cvar_SetValue(r_displayIndex->name, r_displayIndex->integer);
	}

	ri.Printf(PRINT_ALL, " Destroy Window Subsystem.\n");

	ri.IN_Shutdown();
	SDL_QuitSubSystem( SDL_INIT_VIDEO );

    SDL_DestroyWindow( window_sdl );
	if (properties) SDL_DestroyProperties(properties);

    window_sdl = NULL;
	properties = 0;
}


void vk_createSurfaceImpl(void)
{
    ri.Printf(PRINT_ALL, " Create Surface: vk.surface.\n");

    if(!SDL_Vulkan_CreateSurface(window_sdl, vk.instance, NULL, &vk.surface))
    {
        vk.surface = VK_NULL_HANDLE;
        ri.Error(ERR_FATAL, "SDL_Vulkan_CreateSurface(): %s\n", SDL_GetError());
    }
}




/*
===============
Minimize the game so that user is back at the desktop
===============
*/
void vk_minimizeWindow( void )
{
    VkBool32 toggleWorked = 1;
    ri.Printf( PRINT_ALL, " Minimizing Window (SDL). \n");

	VkBool32 isWinFullscreen = ( SDL_GetWindowFlags( window_sdl ) & SDL_WINDOW_FULLSCREEN );
    

    if( isWinFullscreen )
	{
		toggleWorked = (SDL_SetWindowFullscreen( window_sdl, 0 ) >= 0);
	}

    // SDL_WM_ToggleFullScreen didn't work, so do it the slow way
    if( toggleWorked )
    {
        // ri.IN_Shutdown( );
        SDL_MinimizeWindow( window_sdl );
        // SDL_HideWindow( window_sdl );
    }
    else
    {
        ri.Printf( PRINT_ALL, " SDL_SetWindowFullscreen didn't work, so do it the slow way \n");

        ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
    }
}

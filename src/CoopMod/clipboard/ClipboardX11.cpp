/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 */

#if defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

/**
 * SDL 1.2 may load X11 dynamically. Loading the few functions used here in
 * the same way avoids adding a new direct -lX11 dependency to OpenXcom.
 */
class X11Api
{
public:
	typedef Atom (*InternAtomFn)(Display *, const char *, Bool);
	typedef Window (*GetSelectionOwnerFn)(Display *, Atom);
	typedef int (*DeletePropertyFn)(Display *, Window, Atom);
	typedef int (*ConvertSelectionFn)(Display *, Atom, Atom, Atom, Window, Time);
	typedef int (*FlushFn)(Display *);
	typedef Bool (*CheckTypedWindowEventFn)(Display *, Window, int, XEvent *);
	typedef int (*GetWindowPropertyFn)(Display *, Window, Atom, long, long, Bool,
		Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **);
	typedef int (*FreeFn)(void *);

	void *library;
	InternAtomFn internAtom;
	GetSelectionOwnerFn getSelectionOwner;
	DeletePropertyFn deleteProperty;
	ConvertSelectionFn convertSelection;
	FlushFn flush;
	CheckTypedWindowEventFn checkTypedWindowEvent;
	GetWindowPropertyFn getWindowProperty;
	FreeFn freeData;

	X11Api() : library(0), internAtom(0), getSelectionOwner(0),
		deleteProperty(0), convertSelection(0), flush(0),
		checkTypedWindowEvent(0), getWindowProperty(0), freeData(0)
	{
		library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
		if (!library)
		{
			library = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
		}
		if (!library)
		{
			return;
		}

		internAtom = reinterpret_cast<InternAtomFn>(dlsym(library, "XInternAtom"));
		getSelectionOwner = reinterpret_cast<GetSelectionOwnerFn>(dlsym(library, "XGetSelectionOwner"));
		deleteProperty = reinterpret_cast<DeletePropertyFn>(dlsym(library, "XDeleteProperty"));
		convertSelection = reinterpret_cast<ConvertSelectionFn>(dlsym(library, "XConvertSelection"));
		flush = reinterpret_cast<FlushFn>(dlsym(library, "XFlush"));
		checkTypedWindowEvent = reinterpret_cast<CheckTypedWindowEventFn>(dlsym(library, "XCheckTypedWindowEvent"));
		getWindowProperty = reinterpret_cast<GetWindowPropertyFn>(dlsym(library, "XGetWindowProperty"));
		freeData = reinterpret_cast<FreeFn>(dlsym(library, "XFree"));
	}

	~X11Api()
	{
		if (library)
		{
			dlclose(library);
		}
	}

	bool valid() const
	{
		return library && internAtom && getSelectionOwner && deleteProperty &&
			convertSelection && flush && checkTypedWindowEvent &&
			getWindowProperty && freeData;
	}
};

bool requestSelection(X11Api &x11, Display *display, Window window,
	Atom selection, Atom target, Atom property,
	void (*lockDisplay)(void), void (*unlockDisplay)(void),
	std::string &result)
{
	// SDL 1.2 asks callers to use these functions around direct X11 access,
	// but not around X11 event queue functions.
	if (lockDisplay)
	{
		lockDisplay();
	}
	x11.deleteProperty(display, window, property);
	x11.convertSelection(display, selection, target, property, window, CurrentTime);
	x11.flush(display);
	if (unlockDisplay)
	{
		unlockDisplay();
	}

	const Uint32 timeoutAt = SDL_GetTicks() + 750;
	while (static_cast<Sint32>(SDL_GetTicks() - timeoutAt) < 0)
	{
		XEvent event;
		if (x11.checkTypedWindowEvent(display, window, SelectionNotify, &event))
		{
			if (event.xselection.selection != selection ||
				event.xselection.target != target)
			{
				continue;
			}

			if (event.xselection.property == None)
			{
				return false;
			}

			Atom actualType = None;
			int actualFormat = 0;
			unsigned long itemCount = 0;
			unsigned long bytesAfter = 0;
			unsigned char *data = 0;

			// IP addresses, hostnames and ports are tiny. A 1 MiB upper bound
			// also prevents accidentally importing an unreasonable payload.
			const long maxLongs = (1024L * 1024L) / 4L;
			if (lockDisplay)
			{
				lockDisplay();
			}
			const int status = x11.getWindowProperty(display, window, property,
				0, maxLongs, True, AnyPropertyType, &actualType, &actualFormat,
				&itemCount, &bytesAfter, &data);
			if (unlockDisplay)
			{
				unlockDisplay();
			}

			if (status == Success && data && actualFormat == 8 && bytesAfter == 0)
			{
				result.assign(reinterpret_cast<const char *>(data), itemCount);
			}

			if (data)
			{
				x11.freeData(data);
			}

			return !result.empty();
		}

		SDL_Delay(1);
	}

	return false;
}

char *copyToMalloc(const std::string &text)
{
	if (text.empty())
	{
		return 0;
	}

	char *result = static_cast<char *>(std::malloc(text.size() + 1));
	if (!result)
	{
		return 0;
	}

	std::memcpy(result, text.c_str(), text.size() + 1);
	return result;
}

}

extern "C" char *openxcomClipboardGetText()
{
	X11Api x11;
	if (!x11.valid())
	{
		return 0;
	}

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWMInfo(&info))
	{
		return 0;
	}

	Display *display = info.info.x11.display;
	Window window = info.info.x11.window;
	if (!display || !window)
	{
		return 0;
	}

	void (*lockDisplay)(void) = info.info.x11.lock_func;
	void (*unlockDisplay)(void) = info.info.x11.unlock_func;

	if (lockDisplay)
	{
		lockDisplay();
	}
	const Atom clipboard = x11.internAtom(display, "CLIPBOARD", False);
	const Atom property = x11.internAtom(display, "OPENXCOM_CLIPBOARD", False);
	const Atom utf8 = x11.internAtom(display, "UTF8_STRING", True);
	const Window owner = clipboard != None
		? x11.getSelectionOwner(display, clipboard) : None;
	if (unlockDisplay)
	{
		unlockDisplay();
	}

	std::string text;
	if (clipboard != None && property != None && owner != None)
	{
		// Most X11 applications publish UTF8_STRING. XA_STRING is enough
		// as a fallback for the ASCII values used by these fields.
		if (utf8 == None || !requestSelection(x11, display, window,
			clipboard, utf8, property, lockDisplay, unlockDisplay, text))
		{
			text.clear();
			requestSelection(x11, display, window, clipboard, XA_STRING,
				property, lockDisplay, unlockDisplay, text);
		}
	}

	return copyToMalloc(text);
}

#endif

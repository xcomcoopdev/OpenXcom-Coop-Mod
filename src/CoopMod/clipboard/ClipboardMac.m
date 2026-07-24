/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>

char *openxcomClipboardGetText(void)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

	// NSStringPboardType keeps this compatible with the older macOS SDKs
	// normally used by SDL 1.2 builds.
	NSString *text = [pasteboard stringForType:NSStringPboardType];
	char *result = 0;

	if (text)
	{
		const char *utf8 = [text UTF8String];
		if (utf8)
		{
			const size_t length = strlen(utf8) + 1;
			result = (char *)malloc(length);
			if (result)
			{
				memcpy(result, utf8, length);
			}
		}
	}

	[pool drain];
	return result;
}

#endif

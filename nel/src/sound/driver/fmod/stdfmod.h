// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef STDFMOD_H
#define STDFMOD_H

#if defined(_MSC_VER) && defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#include <nel/misc/types_nl.h>

#ifdef NL_OS_WINDOWS
#	pragma include_alias(<fmod.h>, <fmod3\fmod.h>)
#endif
#include <fmod.h>

#include <cmath>
#include <limits>

#include "nel/misc/common.h"
#include "nel/misc/time_nl.h"
#include "nel/misc/singleton.h"
#include "nel/misc/fast_mem.h"
#include "nel/misc/debug.h"
#include "nel/misc/vector.h"
#include "nel/misc/path.h"
#include "nel/misc/file.h"
#include "nel/misc/matrix.h"
#include "nel/misc/big_file.h"
#include "nel/misc/hierarchical_timer.h"
#include "nel/misc/dynloadlib.h"

#include "nel/sound/driver/sound_driver.h"
#include "nel/sound/driver/buffer.h"
#include "nel/sound/driver/source.h"
#include "nel/sound/driver/listener.h"

#endif

/* end of file */

/* nntpgen: generate dummy news articles */
/* 
 * Copyright (c) 2013 River Tarnell.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#ifndef	NNTPGEN_H_INCLUDED
#define	NNTPGEN_H_INCLUDED

#include	<sys/types.h>

#include	"setup.h"

void	*xcalloc(size_t, size_t);
void	*xmalloc(size_t);

#endif	/* !NNTPGEN_H_INCLUDED */

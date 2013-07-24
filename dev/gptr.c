#include <stdio.h>
#include <glib.h>

void main()
{
	GPtrArray *r = g_ptr_array_new();

	gpointer *i = g_ptr_array_index(r, 0);
	if (i == NULL) printf("hi: %d\n",(int)i);
}

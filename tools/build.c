/*
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Martin von Loewis
 * Copyright 1995, 1996 Alexandre Julliard
 */

#ifndef WINELIB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "registers.h"
#include "winerror.h"  /* for ERROR_CALL_NOT_IMPLEMENTED */
#include "module.h"
#include "neexe.h"
#include "windows.h"

/* ELF symbols do not have an underscore in front */
#if defined (__ELF__) || defined (__svr4__) || defined(_SCO_DS)
#define PREFIX
#else
#define PREFIX "_"
#endif

#define TYPE_INVALID     0
#define TYPE_BYTE        1
#define TYPE_WORD        2
#define TYPE_LONG        3
#define TYPE_PASCAL_16   4
#define TYPE_PASCAL      5
#define TYPE_REGISTER    6
#define TYPE_ABS         7
#define TYPE_RETURN      8
#define TYPE_STUB        9
#define TYPE_STDCALL    10

#define MAX_ORDINALS	1299

  /* Callback function used for stub functions */
#define STUB_CALLBACK \
  ((SpecType == SPEC_WIN16) ? "RELAY_Unimplemented16": "RELAY_Unimplemented32")

enum SPEC_TYPE
{
    SPEC_INVALID,
    SPEC_WIN16,
    SPEC_WIN32
};

typedef struct ordinal_definition_s
{
    int type;
    int offset;
    char export_name[80];
    void *additional_data;
} ORDDEF;

typedef struct ordinal_variable_definition_s
{
    int n_values;
    int *values;
} ORDVARDEF;

typedef struct ordinal_function_definition_s
{
    int  n_args;
    char arg_types[32];
    char internal_name[80];
} ORDFUNCDEF;

typedef struct ordinal_return_definition_s
{
    int arg_size;
    int ret_value;
} ORDRETDEF;

static ORDDEF OrdinalDefinitions[MAX_ORDINALS];

static enum SPEC_TYPE SpecType = SPEC_INVALID;
char DLLName[80];
int Limit = 0;
int Base = 0;
int HeapSize = 0;
FILE *SpecFp;

char *ParseBuffer = NULL;
char *ParseNext;
char ParseSaveChar;
int Line;

static int debugging = 1;

  /* Offset of register relative to the end of the context struct */
#define CONTEXTOFFSET(reg) \
  ((int)&reg##_reg((struct sigcontext_struct *)0) \
   - sizeof(struct sigcontext_struct))

static void *xmalloc (size_t size)
{
    void *res;

    res = malloc (size ? size : 1);
    if (res == NULL)
    {
        fprintf (stderr, "Virtual memory exhausted.\n");
        exit (1);
    }
    return res;
}


static void *xrealloc (void *ptr, size_t size)
{
    void *res = realloc (ptr, size);
    if (res == NULL)
    {
        fprintf (stderr, "Virtual memory exhausted.\n");
        exit (1);
    }
    return res;
}


static int IsNumberString(char *s)
{
    while (*s != '\0')
	if (!isdigit(*s++))
	    return 0;

    return 1;
}

static char *strupper(char *s)
{
    char *p;
    
    for(p = s; *p != '\0'; p++)
	*p = toupper(*p);

    return s;
}

static char * GetTokenInLine(void)
{
    char *p;
    char *token;

    if (ParseNext != ParseBuffer)
    {
	if (ParseSaveChar == '\0')
	    return NULL;
	*ParseNext = ParseSaveChar;
    }
    
    /*
     * Remove initial white space.
     */
    for (p = ParseNext; isspace(*p); p++)
	;
    
    if ((*p == '\0') || (*p == '#'))
	return NULL;
    
    /*
     * Find end of token.
     */
    token = p++;
    if (*token != '(' && *token != ')')
	while (*p != '\0' && *p != '(' && *p != ')' && !isspace(*p))
	    p++;
    
    ParseSaveChar = *p;
    ParseNext = p;
    *p = '\0';

    return token;
}

static char * GetToken(void)
{
    char *token;

    if (ParseBuffer == NULL)
    {
	ParseBuffer = xmalloc(512);
	ParseNext = ParseBuffer;
	Line++;
	while (1)
	{
	    if (fgets(ParseBuffer, 511, SpecFp) == NULL)
		return NULL;
	    if (ParseBuffer[0] != '#')
		break;
	}
    }

    while ((token = GetTokenInLine()) == NULL)
    {
	ParseNext = ParseBuffer;
	Line++;
	while (1)
	{
	    if (fgets(ParseBuffer, 511, SpecFp) == NULL)
		return NULL;
	    if (ParseBuffer[0] != '#')
		break;
	}
    }

    return token;
}

static int ParseVariable(int ordinal, int type)
{
    ORDDEF *odp;
    ORDVARDEF *vdp;
    char export_name[80];
    char *token;
    char *endptr;
    int *value_array;
    int n_values;
    int value_array_size;
    
    strcpy(export_name, GetToken());

    token = GetToken();
    if (*token != '(')
    {
	fprintf(stderr, "%d: Expected '(' got '%s'\n", Line, token);
	exit(1);
    }

    n_values = 0;
    value_array_size = 25;
    value_array = xmalloc(sizeof(*value_array) * value_array_size);
    
    while ((token = GetToken()) != NULL)
    {
	if (*token == ')')
	    break;

	value_array[n_values++] = strtol(token, &endptr, 0);
	if (n_values == value_array_size)
	{
	    value_array_size += 25;
	    value_array = xrealloc(value_array, 
				   sizeof(*value_array) * value_array_size);
	}
	
	if (endptr == NULL || *endptr != '\0')
	{
	    fprintf(stderr, "%d: Expected number value, got '%s'\n", Line,
		    token);
	    exit(1);
	}
    }
    
    if (token == NULL)
    {
	fprintf(stderr, "%d: End of file in variable declaration\n", Line);
	exit(1);
    }

    if (ordinal >= MAX_ORDINALS)
    {
	fprintf(stderr, "%d: Ordinal number too large\n", Line);
	exit(1);
    }
    
    odp = &OrdinalDefinitions[ordinal];
    odp->type = type;
    strcpy(odp->export_name, export_name);

    vdp = xmalloc(sizeof(*vdp));
    odp->additional_data = vdp;
    
    vdp->n_values = n_values;
    vdp->values = xrealloc(value_array, sizeof(*value_array) * n_values);

    return 0;
}

static int ParseExportFunction(int ordinal, int type)
{
    char *token;
    ORDDEF *odp;
    ORDFUNCDEF *fdp;
    int i;

    switch(SpecType)
    {
    case SPEC_WIN16:
        if (type == TYPE_STDCALL)
        {
            fprintf( stderr, "%d: 'stdcall' not supported for Win16\n", Line );
            exit(1);
        }
        break;
    case SPEC_WIN32:
        if ((type == TYPE_PASCAL) || (type == TYPE_PASCAL_16))
        {
            fprintf( stderr, "%d: 'pascal' not supported for Win32\n", Line );
            exit(1);
        }
        break;
    default:
        break;
    }
    odp = &OrdinalDefinitions[ordinal];
    strcpy(odp->export_name, GetToken());
    odp->type = type;
    fdp = xmalloc(sizeof(*fdp));
    odp->additional_data = fdp;

    token = GetToken();
    if (*token != '(')
    {
	fprintf(stderr, "%d: Expected '(' got '%s'\n", Line, token);
	exit(1);
    }

    for (i = 0; i < sizeof(fdp->arg_types)-1; i++)
    {
	token = GetToken();
	if (*token == ')')
	    break;

        if (!strcmp(token, "byte") || !strcmp(token, "word"))
            fdp->arg_types[i] = 'w';
        else if (!strcmp(token, "s_byte") || !strcmp(token, "s_word"))
            fdp->arg_types[i] = 's';
        else if (!strcmp(token, "long") || !strcmp(token, "segptr"))
            fdp->arg_types[i] = 'l';
        else if (!strcmp(token, "ptr"))
            fdp->arg_types[i] = 'p';
        else
        {
            fprintf(stderr, "%d: Unknown variable type '%s'\n", Line, token);
            exit(1);
        }
        if (SpecType == SPEC_WIN32)
        {
            if (strcmp(token, "long") && strcmp(token, "ptr"))
            {
                fprintf( stderr, "%d: Type '%s' not supported for Win32\n",
                         Line, token );
                exit(1);
            }
        }
    }
    if (*token != ')')
    {
        fprintf( stderr, "%d: Too many arguments\n", Line );
        exit(1);
    }
    fdp->arg_types[i] = '\0';

    strcpy(fdp->internal_name, GetToken());
    return 0;
}

static int ParseEquate(int ordinal)
{
    ORDDEF *odp;
    char *token;
    char *endptr;
    int value;
    
    odp = &OrdinalDefinitions[ordinal];
    strcpy(odp->export_name, GetToken());

    token = GetToken();
    value = strtol(token, &endptr, 0);
    if (endptr == NULL || *endptr != '\0')
    {
	fprintf(stderr, "%d: Expected number value, got '%s'\n", Line,
		token);
	exit(1);
    }

    odp->type = TYPE_ABS;
    odp->additional_data = (void *) value;

    return 0;
}

static int ParseReturn(int ordinal)
{
    ORDDEF *odp;
    ORDRETDEF *rdp;
    char *token;
    char *endptr;
    
    rdp = xmalloc(sizeof(*rdp));
    
    odp = &OrdinalDefinitions[ordinal];
    strcpy(odp->export_name, GetToken());
    odp->type = TYPE_RETURN;
    odp->additional_data = rdp;

    token = GetToken();
    rdp->arg_size = strtol(token, &endptr, 0);
    if (endptr == NULL || *endptr != '\0')
    {
	fprintf(stderr, "%d: Expected number value, got '%s'\n", Line,
		token);
	exit(1);
    }

    token = GetToken();
    rdp->ret_value = strtol(token, &endptr, 0);
    if (endptr == NULL || *endptr != '\0')
    {
	fprintf(stderr, "%d: Expected number value, got '%s'\n", Line,
		token);
	exit(1);
    }

    return 0;
}


static int ParseStub( int ordinal )
{
    ORDDEF *odp;
    ORDFUNCDEF *fdp;
    
    odp = &OrdinalDefinitions[ordinal];
    strcpy( odp->export_name, GetToken() );
    odp->type = TYPE_STUB;
    fdp = xmalloc(sizeof(*fdp));
    odp->additional_data = fdp;
    fdp->arg_types[0] = '\0';
    strcpy( fdp->internal_name, STUB_CALLBACK );
    return 0;
}


static int ParseOrdinal(int ordinal)
{
    char *token;
    
    if (ordinal >= MAX_ORDINALS)
    {
	fprintf(stderr, "%d: Ordinal number too large\n", Line);
	exit(1);
    }
    if (ordinal > Limit) Limit = ordinal;

    token = GetToken();
    if (token == NULL)
    {
	fprintf(stderr, "%d: Expected type after ordinal\n", Line);
	exit(1);
    }

    if (strcmp(token, "byte") == 0)
	return ParseVariable(ordinal, TYPE_BYTE);
    if (strcmp(token, "word") == 0)
	return ParseVariable(ordinal, TYPE_WORD);
    if (strcmp(token, "long") == 0)
	return ParseVariable(ordinal, TYPE_LONG);
    if (strcmp(token, "pascal") == 0)
	return ParseExportFunction(ordinal, TYPE_PASCAL);
    if (strcmp(token, "pascal16") == 0)
	return ParseExportFunction(ordinal, TYPE_PASCAL_16);
    if (strcmp(token, "register") == 0)
        return ParseExportFunction(ordinal, TYPE_REGISTER);
    if (strcmp(token, "stdcall") == 0)
        return ParseExportFunction(ordinal, TYPE_STDCALL);
    if (strcmp(token, "equate") == 0)
	return ParseEquate(ordinal);
    if (strcmp(token, "return") == 0)
	return ParseReturn(ordinal);
    if (strcmp(token, "stub") == 0)
	return ParseStub(ordinal);
    fprintf(stderr, 
            "%d: Expected type after ordinal, found '%s' instead\n",
            Line, token);
    exit(1);
}

static int ParseTopLevel(void)
{
    char *token;
    
    while ((token = GetToken()) != NULL)
    {
	if (strcmp(token, "name") == 0)
	{
	    strcpy(DLLName, GetToken());
	    strupper(DLLName);
	}
        else if (strcmp(token, "type") == 0)
        {
            token = GetToken();
            if (!strcmp(token, "win16" )) SpecType = SPEC_WIN16;
            else if (!strcmp(token, "win32" )) SpecType = SPEC_WIN32;
            else
            {
                fprintf(stderr, "%d: Type must be 'win16' or 'win32'\n", Line);
                exit(1);
            }
        }
	else if (strcmp(token, "base") == 0)
	{
            token = GetToken();
            if (!IsNumberString(token))
            {
		fprintf(stderr, "%d: Expected number after base\n", Line);
		exit(1);
            }
            Base = atoi(token);
	}
	else if (strcmp(token, "heap") == 0)
	{
            token = GetToken();
            if (!IsNumberString(token))
            {
		fprintf(stderr, "%d: Expected number after heap\n", Line);
		exit(1);
            }
            HeapSize = atoi(token);
	}
	else if (IsNumberString(token))
	{
	    int ordinal;
	    int rv;
	    
	    ordinal = atoi(token);
	    if ((rv = ParseOrdinal(ordinal)) < 0)
		return rv;
	}
	else
	{
	    fprintf(stderr, 
		    "%d: Expected name, id, length or ordinal\n", Line);
	    exit(1);
	}
    }

    return 0;
}


/*******************************************************************
 *         StoreVariableCode
 *
 * Store a list of ints into a byte array.
 */
static int StoreVariableCode( unsigned char *buffer, int size, ORDDEF *odp )
{
    ORDVARDEF *vdp;
    int i;

    vdp = odp->additional_data;
    switch(size)
    {
    case 1:
        for (i = 0; i < vdp->n_values; i++)
            buffer[i] = vdp->values[i];
        break;
    case 2:
        for (i = 0; i < vdp->n_values; i++)
            ((unsigned short *)buffer)[i] = vdp->values[i];
        break;
    case 4:
        for (i = 0; i < vdp->n_values; i++)
            ((unsigned int *)buffer)[i] = vdp->values[i];
        break;
    }
    return vdp->n_values * size;
}


/*******************************************************************
 *         DumpBytes
 *
 * Dump a byte stream into the assembly code.
 */
static void DumpBytes( const unsigned char *data, int len,
                       const char *section, const char *label_start )
{
    int i;
    if (section) printf( "\t%s\n", section );
    if (label_start) printf( "%s:\n", label_start );
    for (i = 0; i < len; i++)
    {
        if (!(i & 0x0f)) printf( "\t.byte " );
        printf( "%d", *data++ );
        if (i < len - 1) printf( "%c", ((i & 0x0f) != 0x0f) ? ',' : '\n' );
    }
    printf( "\n" );
}


/*******************************************************************
 *         BuildModule16
 *
 * Build the in-memory representation of a 16-bit NE module, and dump it
 * as a byte stream into the assembly code.
 */
static int BuildModule16( int max_code_offset, int max_data_offset )
{
    ORDDEF *odp;
    int i;
    char *buffer;
    NE_MODULE *pModule;
    SEGTABLEENTRY *pSegment;
    OFSTRUCT *pFileInfo;
    BYTE *pstr, *bundle;
    WORD *pword;

    /*   Module layout:
     * NE_MODULE       Module
     * OFSTRUCT        File information
     * SEGTABLEENTRY   Segment 1 (code)
     * SEGTABLEENTRY   Segment 2 (data)
     * WORD[2]         Resource table (empty)
     * BYTE[2]         Imported names (empty)
     * BYTE[n]         Resident names table
     * BYTE[n]         Entry table
     */

    buffer = xmalloc( 0x10000 );

    pModule = (NE_MODULE *)buffer;
    pModule->magic = NE_SIGNATURE;
    pModule->count = 1;
    pModule->next = 0;
    pModule->flags = NE_FFLAGS_SINGLEDATA | NE_FFLAGS_BUILTIN | NE_FFLAGS_LIBMODULE;
    pModule->dgroup = 2;
    pModule->heap_size = HeapSize;
    pModule->stack_size = 0;
    pModule->ip = 0;
    pModule->cs = 0;
    pModule->sp = 0;
    pModule->ss = 0;
    pModule->seg_count = 2;
    pModule->modref_count = 0;
    pModule->nrname_size = 0;
    pModule->modref_table = 0;
    pModule->nrname_fpos = 0;
    pModule->moveable_entries = 0;
    pModule->alignment = 0;
    pModule->truetype = 0;
    pModule->os_flags = NE_OSFLAGS_WINDOWS;
    pModule->misc_flags = 0;
    pModule->dlls_to_init  = 0;
    pModule->nrname_handle = 0;
    pModule->min_swap_area = 0;
    pModule->expected_version = 0x030a;
    pModule->pe_module = NULL;
    pModule->self = 0;
    pModule->self_loading_sel = 0;

      /* File information */

    pFileInfo = (OFSTRUCT *)(pModule + 1);
    pModule->fileinfo = (int)pFileInfo - (int)pModule;
    memset( pFileInfo, 0, sizeof(*pFileInfo) - sizeof(pFileInfo->szPathName) );
    pFileInfo->cBytes = sizeof(*pFileInfo) - sizeof(pFileInfo->szPathName)
                        + strlen(DLLName) + 4;
    sprintf( pFileInfo->szPathName, "%s.DLL", DLLName );
    pstr = (char *)pFileInfo + pFileInfo->cBytes + 1;
        
      /* Segment table */

    pSegment = (SEGTABLEENTRY *)pstr;
    pModule->seg_table = (int)pSegment - (int)pModule;
    pSegment->filepos = 0;
    pSegment->size = max_code_offset;
    pSegment->flags = 0;
    pSegment->minsize = max_code_offset;
    pSegment->selector = 0;
    pSegment++;

    pModule->dgroup_entry = (int)pSegment - (int)pModule;
    pSegment->filepos = 0;
    pSegment->size = max_data_offset;
    pSegment->flags = NE_SEGFLAGS_DATA;
    pSegment->minsize = max_data_offset;
    pSegment->selector = 0;
    pSegment++;

      /* Resource table */

    pword = (WORD *)pSegment;
    pModule->res_table = (int)pword - (int)pModule;
    *pword++ = 0;
    *pword++ = 0;

      /* Imported names table */

    pstr = (char *)pword;
    pModule->import_table = (int)pstr - (int)pModule;
    *pstr++ = 0;
    *pstr++ = 0;

      /* Resident names table */

    pModule->name_table = (int)pstr - (int)pModule;
    /* First entry is module name */
    *pstr = strlen(DLLName );
    strcpy( pstr + 1, DLLName );
    pstr += *pstr + 1;
    *(WORD *)pstr = 0;
    pstr += sizeof(WORD);
    /* Store all ordinals */
    odp = OrdinalDefinitions + 1;
    for (i = 1; i <= Limit; i++, odp++)
    {
        if (!odp->export_name[0]) continue;
        *pstr = strlen( odp->export_name );
        strcpy( pstr + 1, odp->export_name );
        strupper( pstr + 1 );
        pstr += *pstr + 1;
        *(WORD *)pstr = i;
        pstr += sizeof(WORD);
    }
    *pstr++ = 0;

      /* Entry table */

    pModule->entry_table = (int)pstr - (int)pModule;
    bundle = NULL;
    odp = OrdinalDefinitions + 1;
    for (i = 1; i <= Limit; i++, odp++)
    {
        int selector = 0;

	switch (odp->type)
	{
        case TYPE_INVALID:
            selector = 0;  /* Invalid selector */
            break;

        case TYPE_PASCAL:
        case TYPE_PASCAL_16:
        case TYPE_REGISTER:
        case TYPE_RETURN:
        case TYPE_STUB:
            selector = 1;  /* Code selector */
            break;

        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_LONG:
            selector = 2;  /* Data selector */
            break;

        case TYPE_ABS:
            selector = 0xfe;  /* Constant selector */
            break;
        }

          /* create a new bundle if necessary */
        if (!bundle || (bundle[0] >= 254) || (bundle[1] != selector))
        {
            bundle = pstr;
            bundle[0] = 0;
            bundle[1] = selector;
            pstr += 2;
        }

        (*bundle)++;
        if (selector != 0)
        {
            *pstr++ = 1;
            *(WORD *)pstr = odp->offset;
            pstr += sizeof(WORD);
        }
    }
    *pstr++ = 0;

      /* Dump the module content */

    DumpBytes( (char *)pModule, (int)pstr - (int)pModule,
               ".data", "Module_Start" );
    return (int)pstr - (int)pModule;
}


/*******************************************************************
 *         BuildModule32
 *
 * Build the in-memory representation of a 32-bit pseudo-NE module, and dump it
 * as a byte stream into the assembly code.
 */
static int BuildModule32(void)
{
    char *buffer;
    NE_MODULE *pModule;
    OFSTRUCT *pFileInfo;
    BYTE *pstr;
    WORD *pword;

    /*   Module layout:
     * NE_MODULE            Module
     * OFSTRUCT             File information
     * SEGTABLEENTRY        Segment table (empty)
     * WORD[2]              Resource table (empty)
     * BYTE[2]              Imported names (empty)
     * BYTE[n]              Resident names table (1 entry)
     * BYTE[n]              Entry table (empty)
     */

    buffer = xmalloc( 0x10000 );

    pModule = (NE_MODULE *)buffer;
    pModule->magic = NE_SIGNATURE;
    pModule->count = 1;
    pModule->next = 0;
    pModule->flags = NE_FFLAGS_SINGLEDATA | NE_FFLAGS_BUILTIN |
                     NE_FFLAGS_LIBMODULE | NE_FFLAGS_WIN32;
    pModule->dgroup = 0;
    pModule->heap_size = HeapSize;
    pModule->stack_size = 0;
    pModule->ip = 0;
    pModule->cs = 0;
    pModule->sp = 0;
    pModule->ss = 0;
    pModule->seg_count = 0;
    pModule->modref_count = 0;
    pModule->nrname_size = 0;
    pModule->modref_table = 0;
    pModule->nrname_fpos = 0;
    pModule->moveable_entries = 0;
    pModule->alignment = 0;
    pModule->truetype = 0;
    pModule->os_flags = NE_OSFLAGS_WINDOWS;
    pModule->misc_flags = 0;
    pModule->dlls_to_init  = 0;
    pModule->nrname_handle = 0;
    pModule->min_swap_area = 0;
    pModule->expected_version = 0x030a;
    pModule->pe_module = NULL;
    pModule->self = 0;
    pModule->self_loading_sel = 0;

      /* File information */

    pFileInfo = (OFSTRUCT *)(pModule + 1);
    pModule->fileinfo = (int)pFileInfo - (int)pModule;
    memset( pFileInfo, 0, sizeof(*pFileInfo) - sizeof(pFileInfo->szPathName) );
    pFileInfo->cBytes = sizeof(*pFileInfo) - sizeof(pFileInfo->szPathName)
                        + strlen(DLLName) + 4;
    sprintf( pFileInfo->szPathName, "%s.DLL", DLLName );
    pstr = (char *)pFileInfo + pFileInfo->cBytes + 1;
        
      /* Segment table */

    pModule->seg_table = (int)pstr - (int)pModule;

      /* Resource table */

    pword = (WORD *)pstr;
    pModule->res_table = (int)pword - (int)pModule;
    *pword++ = 0;
    *pword++ = 0;

      /* Imported names table */

    pstr = (char *)pword;
    pModule->import_table = (int)pstr - (int)pModule;
    *pstr++ = 0;
    *pstr++ = 0;

      /* Resident names table */

    pModule->name_table = (int)pstr - (int)pModule;
    /* First entry is module name */
    *pstr = strlen(DLLName );
    strcpy( pstr + 1, DLLName );
    pstr += *pstr + 1;
    *(WORD *)pstr = 0;
    pstr += sizeof(WORD);
    *pstr++ = 0;

      /* Entry table */

    pModule->entry_table = (int)pstr - (int)pModule;
    *pstr++ = 0;

      /* Dump the module content */

    DumpBytes( (char *)pModule, (int)pstr - (int)pModule,
               ".data", "Module_Start" );
    return (int)pstr - (int)pModule;
}


/*******************************************************************
 *         BuildSpec32Files
 *
 * Build a Win32 assembly file from a spec file.
 */
static void BuildSpec32Files(void)
{
    ORDDEF *odp;
    ORDFUNCDEF *fdp;
    ORDRETDEF *rdp;
    int i, module_size;

    printf( "/* File generated automatically; do not edit! */\n" );
    printf( "\t.text\n" );
    printf( "\t.align 4\n" );
    printf( "Code_Start:\n\n" );

    odp = OrdinalDefinitions;
    for (i = 0; i <= Limit; i++, odp++)
    {
        fdp = odp->additional_data;
        rdp = odp->additional_data;

        switch (odp->type)
        {
        case TYPE_INVALID:
            printf( "/* %s.%d */\n", DLLName, i );
            printf( "\t.align 4\n" );
            printf( "%s_%d:\n", DLLName, i );
            printf( "\tpushl %%ebp\n" );
            printf( "\tpushl $Name_%d\n", i );
            printf( "\tpushl $" PREFIX "%s\n", STUB_CALLBACK );
            printf( "\tjmp " PREFIX "CallFrom32_0\n" );
            break;

        case TYPE_STDCALL:
        case TYPE_STUB:
            printf( "/* %s.%d (%s) */\n",
                     DLLName, i, odp->export_name);
            printf( "\t.align 4\n" );
            printf( "%s_%d:\n", DLLName, i );
            printf( "\tpushl %%ebp\n" );
            printf( "\tpushl $Name_%d\n", i );
            printf( "\tpushl $" PREFIX "%s\n", fdp->internal_name );
            printf( "\tjmp " PREFIX "CallFrom32_%d\n", strlen(fdp->arg_types));
            break;

        case TYPE_RETURN:
            printf( "/* %s.%d (%s) */\n",
                     DLLName, i, odp->export_name);
            printf( "\t.align 4\n" );
            printf( "%s_%d:\n", DLLName, i );
            printf( "\tmovl $%d,%%eax\n", ERROR_CALL_NOT_IMPLEMENTED );
            printf( "\tmovl %%eax," PREFIX "WIN32_LastError\n" );
            printf( "\tmovl $%d,%%eax\n", rdp->ret_value );
            if (rdp->arg_size) printf( "\tret $%d\n", rdp->arg_size );
            else printf( "\tret\n" );
            break;

        default:
            fprintf(stderr,"build: function type %d not available for Win32\n",
                    odp->type);
            exit(1);
        }
    }

    module_size = BuildModule32();

    /* Output the DLL functions table */

    printf( "\t.text\n" );
    printf( "\t.align 4\n" );
    printf( "Functions:\n" );
    odp = OrdinalDefinitions;
    for (i = 0; i <= Limit; i++, odp++) printf("\t.long %s_%d\n", DLLName, i);

    /* Output the DLL names table */

    printf( "FuncNames:\n" );
    odp = OrdinalDefinitions;
    for (i = 0; i <= Limit; i++, odp++)
    {
        if (odp->type == TYPE_INVALID) printf( "\t.long 0\n" );
        else printf( "\t.long Name_%d\n", i );
    }

    /* Output the DLL names */

    for (i = 0, odp = OrdinalDefinitions; i <= Limit; i++, odp++)
    {
        printf( "Name_%d:\t", i );
        if (odp->type == TYPE_INVALID)
            printf( ".ascii \"%s.%d\\0\"\n", DLLName, i );
        else
            printf( ".ascii \"%s\\0\"\n", odp->export_name );
    }

    /* Output the DLL descriptor */

    printf( "DLLName:\t.ascii \"%s\\0\"\n", DLLName );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "%s_Descriptor\n", DLLName );
    printf( PREFIX "%s_Descriptor:\n", DLLName );
    printf( "\t.long DLLName\n" );          /* Name */
    printf( "\t.long Module_Start\n" );     /* Module start */
    printf( "\t.long %d\n", module_size );  /* Module size */
    printf( "\t.long %d\n", Base );         /* Base */
    printf( "\t.long %d\n", Limit );        /* Limit */
    printf( "\t.long Functions\n" );        /* Functions */
    printf( "\t.long FuncNames\n" );        /* Function names */
}


/*******************************************************************
 *         BuildSpec16Files
 *
 * Build a Win16 assembly file from a spec file.
 */
static void BuildSpec16Files(void)
{
    ORDDEF *odp;
    ORDFUNCDEF *fdp;
    ORDRETDEF *rdp;
    int i;
    int code_offset, data_offset, module_size;
    unsigned char *data;

    data = (unsigned char *)xmalloc( 0x10000 );
    memset( data, 0, 16 );
    data_offset = 16;

    printf( "/* File generated automatically; do not edit! */\n" );
    printf( "\t.text\n" );
    printf( "Code_Start:\n" );
    code_offset = 0;

    odp = OrdinalDefinitions;
    for (i = 0; i <= Limit; i++, odp++)
    {
        fdp = odp->additional_data;
        rdp = odp->additional_data;
	    
        switch (odp->type)
        {
          case TYPE_INVALID:
            odp->offset = 0xffff;
            break;

          case TYPE_ABS:
            odp->offset = (int)odp->additional_data & 0xffff;
            break;

          case TYPE_BYTE:
            odp->offset = data_offset;
            data_offset += StoreVariableCode( data + data_offset, 1, odp);
            break;

          case TYPE_WORD:
            odp->offset = data_offset;
            data_offset += StoreVariableCode( data + data_offset, 2, odp);
            break;

          case TYPE_LONG:
            odp->offset = data_offset;
            data_offset += StoreVariableCode( data + data_offset, 4, odp);
            break;

          case TYPE_RETURN:
            printf( "/* %s.%d */\n", DLLName, i);
            printf( "\tmovw $%d,%%ax\n", rdp->ret_value & 0xffff );
            printf( "\tmovw $%d,%%dx\n", (rdp->ret_value >> 16) & 0xffff);
            printf( "\t.byte 0x66\n");
            if (rdp->arg_size != 0)
                printf( "\tlret $%d\n\n", rdp->arg_size);
            else
            {
                printf( "\tlret\n");
                printf( "\tnop\n");
                printf( "\tnop\n\n");
            }
            odp->offset = code_offset;
            code_offset += 12;  /* Assembly code is 12 bytes long */
            break;

          case TYPE_REGISTER:
          case TYPE_PASCAL:
          case TYPE_PASCAL_16:
          case TYPE_STUB:
            printf( "/* %s.%d */\n", DLLName, i);
            printf( "\tpushw %%bp\n" );
            printf( "\tpushl $" PREFIX "%s\n", fdp->internal_name );
            /* FreeBSD does not understand lcall, so do it the hard way */
            printf( "\t.byte 0x9a /*lcall*/\n" );
            printf( "\t.long " PREFIX "CallFrom16_%s_%s\n",
                    (odp->type == TYPE_REGISTER) ? "regs" :
                    (odp->type == TYPE_PASCAL) ? "long" : "word",
                    fdp->arg_types );
            printf( "\t.byte 0x%02x,0x%02x\n", /* Some asms don't have .word */
                    LOBYTE(WINE_CODE_SELECTOR), HIBYTE(WINE_CODE_SELECTOR) );
            printf( "\tnop\n" );
            printf( "\tnop\n\n" );
            odp->offset = code_offset;
            code_offset += 16;  /* Assembly code is 16 bytes long */
            break;
		
          default:
            fprintf(stderr,"build: function type %d not available for Win16\n",
                    odp->type);
            exit(1);
	}
    }

    if (!code_offset)  /* Make sure the code segment is not empty */
    {
        printf( "\t.byte 0\n" );
        code_offset++;
    }

    /* Output data segment */

    DumpBytes( data, data_offset, NULL, "Data_Start" );

    /* Build the module */

    module_size = BuildModule16( code_offset, data_offset );

    /* Output the DLL descriptor */

    printf( "\t.text\n" );
    printf( "DLLName:\t.ascii \"%s\\0\"\n", DLLName );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "%s_Descriptor\n", DLLName );
    printf( PREFIX "%s_Descriptor:\n", DLLName );
    printf( "\t.long DLLName\n" );          /* Name */
    printf( "\t.long Module_Start\n" );     /* Module start */
    printf( "\t.long %d\n", module_size );  /* Module size */
    printf( "\t.long Code_Start\n" );       /* Code start */
    printf( "\t.long Data_Start\n" );       /* Data start */
}


/*******************************************************************
 *         BuildSpecFiles
 *
 * Build an assembly file from a spec file.
 */
static void BuildSpecFiles( char *specname )
{
    SpecFp = fopen( specname, "r");
    if (SpecFp == NULL)
    {
	fprintf(stderr, "Could not open specification file, '%s'\n", specname);
	exit(1);
    }

    ParseTopLevel();
    switch(SpecType)
    {
    case SPEC_INVALID:
        fprintf( stderr, "%s: Missing 'type' declaration\n", specname );
        exit(1);
    case SPEC_WIN16:
        BuildSpec16Files();
        break;
    case SPEC_WIN32:
        BuildSpec32Files();
        break;
    }
}


/*******************************************************************
 *         BuildCall32LargeStack
 *
 * Build the function used to switch to the original 32-bit stack
 * before calling a 32-bit function from 32-bit code. This is used for
 * functions that need a large stack, like X bitmaps functions.
 *
 * The generated function has the following prototype:
 *   int CallTo32_LargeStack( int (*func)(), int nbargs, ... )
 *
 * Stack layout:
 *   ...     ...
 * (ebp+20)  arg2
 * (ebp+16)  arg1
 * (ebp+12)  nbargs
 * (ebp+8)   func
 * (ebp+4)   ret addr
 * (ebp)     ebp
 */
static void BuildCall32LargeStack(void)
{
    /* Function header */

    printf( "/**********\n" );
    printf( " * " PREFIX "CallTo32_LargeStack\n" );
    printf( " **********/\n" );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "CallTo32_LargeStack\n\n" );
    printf( PREFIX "CallTo32_LargeStack:\n" );
    
    /* Entry code */

    printf( "\tpushl %%ebp\n" );
    printf( "\tmovl %%esp,%%ebp\n" );

    /* Save registers */

    printf( "\tpushl %%ecx\n" );
    printf( "\tpushl %%esi\n" );
    printf( "\tpushl %%edi\n" );

    /* Retrieve the original 32-bit stack pointer and switch to it if any */

    printf( "\tmovl " PREFIX "IF1632_Original32_esp, %%eax\n" );
    printf( "\torl %%eax,%%eax\n" );
    printf( "\tje no_orig_esp\n" );
    printf( "\tmovl %%eax,%%esp\n" );
    printf( "no_orig_esp:\n" );

    /* Transfer the arguments */

    printf( "\tmovl 12(%%ebp),%%ecx\n" );
    printf( "\torl %%ecx,%%ecx\n" );
    printf( "\tje no_args\n" );
    printf( "\tleal 16(%%ebp),%%esi\n" );
    printf( "\tshll $2,%%ecx\n" );
    printf( "\tsubl %%ecx,%%esp\n" );
    printf( "\tmovl %%esp,%%edi\n" );
    printf( "\tshrl $2,%%ecx\n" );
    printf( "\tcld\n" );
    printf( "\trep; movsl\n" );
    printf( "no_args:\n" );

    /* Call the function */

    printf( "\tcall 8(%%ebp)\n" );

    /* Switch back to the normal stack */

    printf( "\tleal -12(%%ebp),%%esp\n" );

    /* Restore registers and return */

    printf( "\tpopl %%edi\n" );
    printf( "\tpopl %%esi\n" );
    printf( "\tpopl %%ecx\n" );
    printf( "\tpopl %%ebp\n" );
    printf( "\tret\n" );
}


/*******************************************************************
 *         TransferArgs16To32
 *
 * Get the arguments from the 16-bit stack and push them on the 32-bit stack.
 * The 16-bit stack layout is:
 *   ...     ...
 *  (bp+8)    arg2
 *  (bp+6)    arg1
 *  (bp+4)    cs
 *  (bp+2)    ip
 *  (bp)      bp
 */
static int TransferArgs16To32( char *args )
{
    int i, pos16, pos32;

    /* Save ebx first */

    printf( "\tpushl %%ebx\n" );

    /* Get the 32-bit stack pointer */

    printf( "\tmovl " PREFIX "IF1632_Saved32_esp,%%ebx\n" );

    /* Copy the arguments */

    pos16 = 6;  /* skip bp and return address */
    pos32 = 0;

    for (i = strlen(args); i > 0; i--)
    {
        pos32 -= 4;
        switch(args[i-1])
        {
        case 'w':  /* word */
            printf( "\tmovzwl %d(%%ebp),%%eax\n", pos16 );
            printf( "\tmovl %%eax,%d(%%ebx)\n", pos32 );
            pos16 += 2;
            break;

        case 's':  /* s_word */
            printf( "\tmovswl %d(%%ebp),%%eax\n", pos16 );
            printf( "\tmovl %%eax,%d(%%ebx)\n", pos32 );
            pos16 += 2;
            break;

        case 'l':  /* long */
            printf( "\tmovl %d(%%ebp),%%eax\n", pos16 );
            printf( "\tmovl %%eax,%d(%%ebx)\n", pos32 );
            pos16 += 4;
            break;

        case 'p':  /* ptr */
            /* Get the selector */
            printf( "\tmovw %d(%%ebp),%%ax\n", pos16 + 2 );
            /* Get the selector base */
            printf( "\tandl $0xfff8,%%eax\n" );
            printf( "\tmovl " PREFIX "ldt_copy(%%eax),%%eax\n" );
            printf( "\tmovl %%eax,%d(%%ebx)\n", pos32 );
            /* Add the offset */
            printf( "\tmovzwl %d(%%ebp),%%eax\n", pos16 );
            printf( "\taddl %%eax,%d(%%ebx)\n", pos32 );
            pos16 += 4;
            break;

        default:
            fprintf( stderr, "Unknown arg type '%c'\n", args[i-1] );
        }
    }

    /* Restore ebx */
    
    printf( "\tpopl %%ebx\n" );

    return pos16 - 6;  /* Return the size of the 16-bit args */
}


/*******************************************************************
 *         BuildContext
 *
 * Build the context structure on the 32-bit stack.
 * The only valid registers in the context structure are:
 *   eax, ebx, ecx, edx, esi, edi, ds, es, (some of the) flags
 */
static void BuildContext(void)
{
    /* Save ebx first */

    printf( "\tpushl %%ebx\n" );

    /* Get the 32-bit stack pointer */

    printf( "\tmovl " PREFIX "IF1632_Saved32_esp,%%ebx\n" );

    /* Store the registers */

    printf( "\tpopl %d(%%ebx)\n", CONTEXTOFFSET(EBX) ); /* Get ebx from stack*/
    printf( "\tmovl %%eax,%d(%%ebx)\n", CONTEXTOFFSET(EAX) );
    printf( "\tmovl %%ecx,%d(%%ebx)\n", CONTEXTOFFSET(ECX) );
    printf( "\tmovl %%edx,%d(%%ebx)\n", CONTEXTOFFSET(EDX) );
    printf( "\tmovl %%esi,%d(%%ebx)\n", CONTEXTOFFSET(ESI) );
    printf( "\tmovl %%edi,%d(%%ebx)\n", CONTEXTOFFSET(EDI) );
    printf( "\tmovw -10(%%ebp),%%ax\n" );  /* Get saved ds from stack */
    printf( "\tmovw %%ax,%d(%%ebx)\n", CONTEXTOFFSET(DS) );
    printf( "\tmovw -6(%%ebp),%%ax\n" );  /* Get saved es from stack */
    printf( "\tmovw %%ax,%d(%%ebx)\n", CONTEXTOFFSET(ES) );
    printf( "\tpushfl\n" );
    printf( "\tpopl %d(%%ebx)\n", CONTEXTOFFSET(EFL) );
}


/*******************************************************************
 *         RestoreContext
 *
 * Restore the registers from the context structure
 */
static void RestoreContext(void)
{
    /* Get the 32-bit stack pointer */

    printf( "\tmovl " PREFIX "IF1632_Saved32_esp,%%ebx\n" );

    /* Restore the registers */

    printf( "\tmovl %d(%%ebx),%%ecx\n", CONTEXTOFFSET(ECX) );
    printf( "\tmovl %d(%%ebx),%%edx\n", CONTEXTOFFSET(EDX) );
    printf( "\tmovl %d(%%ebx),%%esi\n", CONTEXTOFFSET(ESI) );
    printf( "\tmovl %d(%%ebx),%%edi\n", CONTEXTOFFSET(EDI) );
    printf( "\tpopl %%eax\n" );  /* Remove old ds and ip from stack */
    printf( "\tpopl %%eax\n" );  /* Remove old cs and es from stack */
    printf( "\tpushw %d(%%ebx)\n", CONTEXTOFFSET(DS) ); /* Push new ds */
    printf( "\tpushw %d(%%ebx)\n", CONTEXTOFFSET(ES) ); /* Push new es */
    printf( "\tpushl %d(%%ebx)\n", CONTEXTOFFSET(EFL) );
    printf( "\tpopfl\n" );
    printf( "\tmovl %d(%%ebx),%%eax\n", CONTEXTOFFSET(EAX) );
    printf( "\tmovl %d(%%ebx),%%ebx\n", CONTEXTOFFSET(EBX) );
}


/*******************************************************************
 *         BuildCallFrom16Func
 *
 * Build a 16-bit-to-Wine callback function. The syntax of the function
 * profile is: type_xxxxx, where 'type' is one of 'regs', 'word' or
 * 'long' and each 'x' is an argument ('w'=word, 's'=signed word,
 * 'l'=long, 'p'=pointer).
 * For register functions, the arguments are ignored, but they are still
 * removed from the stack upon return.
 *
 * Stack layout upon entry to the callback function:
 *  ...           ...
 * (sp+18) word   first 16-bit arg
 * (sp+16) word   cs
 * (sp+14) word   ip
 * (sp+12) word   bp
 * (sp+8)  long   32-bit entry point
 * (sp+6)  word   high word of cs (always 0, used to store es)
 * (sp+4)  word   low word of cs of 16-bit entry point
 * (sp+2)  word   high word of ip (always 0, used to store ds)
 * (sp)    word   low word of ip of 16-bit entry point
 *
 */
static void BuildCallFrom16Func( char *profile )
{
    int argsize = 0;
    int short_ret = 0;
    int reg_func = 0;
    char *args = profile + 5;

    /* Parse function type */

    if (!strncmp( "word_", profile, 5 )) short_ret = 1;
    else if (!strncmp( "regs_", profile, 5 )) reg_func = 1;
    else if (strncmp( "long_", profile, 5 ))
    {
        fprintf( stderr, "Invalid function name '%s', ignored\n", profile );
        return;
    }

    /* Function header */

    printf( "/**********\n" );
    printf( " * " PREFIX "CallFrom16_%s\n", profile );
    printf( " **********/\n" );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "CallFrom16_%s\n\n", profile );
    printf( PREFIX "CallFrom16_%s:\n", profile );

    /* Setup bp to point to its copy on the stack */

    printf( "\tmovzwl %%sp,%%ebp\n" );
    printf( "\taddw $12,%%bp\n" );

    /* Save 16-bit ds and es */

    /* Stupid FreeBSD assembler doesn't know these either */
    /* printf( "\tmovw %%ds,-10(%%ebp)\n" ); */
    printf( "\t.byte 0x66,0x8c,0x5d,0xf6\n" );
    /* printf( "\tmovw %%es,-6(%%ebp)\n" ); */
    printf( "\t.byte 0x66,0x8c,0x45,0xfa\n" );

    /* Restore 32-bit ds and es */

    printf( "\tpushl $0x%04x%04x\n", WINE_DATA_SELECTOR, WINE_DATA_SELECTOR );
    printf( "\tpopw %%ds\n" );
    printf( "\tpopw %%es\n" );


    /* Save the 16-bit stack */

    printf( "\tpushw " PREFIX "IF1632_Saved16_sp\n" );
    printf( "\tpushw " PREFIX "IF1632_Saved16_ss\n" );
#ifdef __svr4__
    printf("\tdata16\n");
#endif
    printf( "\tmovw %%ss," PREFIX "IF1632_Saved16_ss\n" );
    printf( "\tmovw %%sp," PREFIX "IF1632_Saved16_sp\n" );

    /* Transfer the arguments */

    if (reg_func) BuildContext();
    else if (*args) argsize = TransferArgs16To32( args );

    /* Get the address of the API function */

    printf( "\tmovl -4(%%ebp),%%eax\n" );

    /* If necessary, save %edx over the API function address */

    if (!reg_func && short_ret)
        printf( "\tmovl %%edx,-4(%%ebp)\n" );

    /* Switch to the 32-bit stack */

    printf( "\tmovl " PREFIX "IF1632_Saved32_esp,%%ebp\n" );
    printf( "\tpushw %%ds\n" );
    printf( "\tpopw %%ss\n" );
    printf( "\tleal -%d(%%ebp),%%esp\n",
            reg_func ? sizeof(struct sigcontext_struct) : 4 * strlen(args) );

    /* Setup %ebp to point to the previous stack frame (built by CallTo16) */

    printf( "\taddl $24,%%ebp\n" );

    /* Print the debug information before the call */

    if (debugging)
    {
        printf( "\tpushl %%eax\n" );
        printf( "\tpushl $Profile_%s\n", profile );
        printf( "\tpushl $%d\n", reg_func ? 2 : (short_ret ? 1 : 0) );
        printf( "\tcall " PREFIX "RELAY_DebugCallFrom16\n" );
        printf( "\tpopl %%eax\n" );
        printf( "\tpopl %%eax\n" );
        printf( "\tpopl %%eax\n" );
    }

    /* Call the entry point */

    printf( "\tcall %%eax\n" );

    /* Print the debug information after the call */

    if (debugging)
    {
        printf( "\tpushl %%eax\n" );
        printf( "\tpushl $%d\n", reg_func ? 2 : (short_ret ? 1 : 0) );
        printf( "\tcall " PREFIX "RELAY_DebugCallFrom16Ret\n" );
        printf( "\tpopl %%eax\n" );
        printf( "\tpopl %%eax\n" );
    }

    /* Restore the 16-bit stack */

#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tmovw " PREFIX "IF1632_Saved16_ss,%%ss\n" );
    printf( "\tmovw " PREFIX "IF1632_Saved16_sp,%%sp\n" );
#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tpopw " PREFIX "IF1632_Saved16_ss\n" );
#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tpopw " PREFIX "IF1632_Saved16_sp\n" );

    if (reg_func)
    {
        /* Restore registers from the context structure */
        RestoreContext();
        
        /* Calc the arguments size */
        while (*args)
        {
            switch(*args)
            {
            case 'w':
            case 's':
                argsize += 2;
                break;
            case 'p':
            case 'l':
                argsize += 4;
                break;
            default:
                fprintf( stderr, "Unknown arg type '%c'\n", *args );
            }
            args++;
        }

        /* Restore ds and es */
        printf( "\tpopw %%es\n" );
        printf( "\tpopw %%ds\n" );

        /* Remove the entry point from the stack */
        /* (we don't use add to avoid modifying the carry flag) */
        printf( "\tpopl %%ebp\n" );
    }
    else
    {
        /* Restore ds and es */
        printf( "\tpopw %%bp\n" );       /* Remove ip */
        printf( "\tpopl %%ebp\n" );      /* Remove ds and cs */
        printf( "\tmovw %%bp,%%ds\n" );  /* Restore ds */
        printf( "\tpopw %%es\n" );       /* Restore es */

        if (short_ret) printf( "\tpopl %%edx\n" );     /* Restore edx */
        else
        {
            /* Get the return value into dx:ax */
            printf( "\tpushl %%eax\n" );
            printf( "\tpopw %%ax\n" );
            printf( "\tpopw %%dx\n" );
            /* Remove API entry point */
            printf( "\taddl $4,%%esp\n" );
        }
    }

    /* Restore bp */

    printf( "\tpopw %%bp\n" );

    /* Remove the arguments and return */

    if (argsize)
    {
        printf( "\t.byte 0x66\n" );
        printf( "\tlret $%d\n", argsize );
    }
    else
    {
        printf( "\t.byte 0x66\n" );
        printf( "\tlret\n" );
    }
}


/*******************************************************************
 *         BuildCallTo16Func
 *
 * Build a Wine-to-16-bit callback function.
 *
 * Stack frame of the callback function:
 *  ...      ...
 * (ebp+24) arg2
 * (ebp+20) arg1
 * (ebp+16) 16-bit ds
 * (ebp+12) func to call
 * (ebp+8)  code selector
 * (ebp+4)  return address
 * (ebp)    previous ebp
 *
 * Prototypes for the CallTo16 functions:
 *   extern WORD CallTo16_word_xxx( FARPROC func, WORD ds, args... );
 *   extern LONG CallTo16_long_xxx( FARPROC func, WORD ds, args... );
 *   extern void CallTo16_regs_( FARPROC func, WORD ds, WORD es, WORD bp,
 *                               WORD ax, WORD bx, WORD cx, WORD dx,
 *                               WORD si, WORD di );
 */
static void BuildCallTo16Func( char *profile )
{
    int short_ret = 0;
    int reg_func = 0;
    char *args = profile + 5;

    if (!strncmp( "word_", profile, 5 )) short_ret = 1;
    else if (!strncmp( "regs_", profile, 5 )) reg_func = short_ret = 1;
    else if (strncmp( "long_", profile, 5 ))
    {
        fprintf( stderr, "Invalid function name '%s', ignored\n", profile );
        return;
    }

    /* Function header */

    printf( "/**********\n" );
    printf( " * " PREFIX "CallTo16_%s\n", profile );
    printf( " **********/\n" );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "CallTo16_%s\n\n", profile );
    printf( PREFIX "CallTo16_%s:\n", profile );

    /* Push code selector before return address to simulate a lcall */

    printf( "\tpopl %%eax\n" );
    printf( "\tpushl $0x%04x\n", WINE_CODE_SELECTOR );
    printf( "\tpushl %%eax\n" );

    /* Entry code */

    printf( "\tpushl %%ebp\n" );
    printf( "\tmovl %%esp,%%ebp\n" );

    /* Save the 32-bit registers */

    printf( "\tpushl %%ebx\n" );
    printf( "\tpushl %%ecx\n" );
    printf( "\tpushl %%edx\n" );
    printf( "\tpushl %%esi\n" );
    printf( "\tpushl %%edi\n" );

    /* Save the 32-bit stack */

    printf( "\tpushl " PREFIX "IF1632_Saved32_esp\n" );
    printf( "\tmovl %%esp," PREFIX "IF1632_Saved32_esp\n" );
    printf( "\tmovl %%ebp,%%ebx\n" );

    /* Print debugging info */

    if (debugging)
    {
        /* Push the address of the first argument */
        printf( "\tmovl %%ebx,%%eax\n" );
        printf( "\taddl $12,%%eax\n" );
        printf( "\tpushl $%d\n", reg_func ? 8 : strlen(args) );
        printf( "\tpushl %%eax\n" );
        printf( "\tcall " PREFIX "RELAY_DebugCallTo16\n" );
        printf( "\tpopl %%eax\n" );
        printf( "\tpopl %%eax\n" );
    }

    /* Switch to the 16-bit stack */

#ifdef __svr4__
    printf("\tdata16\n");
#endif
    printf( "\tmovw " PREFIX "IF1632_Saved16_ss,%%ss\n" );
    printf( "\tmovw " PREFIX "IF1632_Saved16_sp,%%sp\n" );

    /* Transfer the arguments */

    if (reg_func)
    {
        /* Get the registers. ebx is handled later on. */
        printf( "\tpushw 20(%%ebx)\n" );
        printf( "\tpopw %%es\n" );
        printf( "\tmovl 24(%%ebx),%%ebp\n" );
        printf( "\tmovl 28(%%ebx),%%eax\n" );
        printf( "\tmovl 36(%%ebx),%%ecx\n" );
        printf( "\tmovl 40(%%ebx),%%edx\n" );
        printf( "\tmovl 44(%%ebx),%%esi\n" );
        printf( "\tmovl 48(%%ebx),%%edi\n" );
    }
    else  /* not a register function */
    {
        int pos = 20;  /* first argument position */

        /* Make %bp point to the previous stackframe (built by CallFrom16) */
        printf( "\tmovw %%sp,%%bp\n" );
        printf( "\taddw $16,%%bp\n" );

        while (*args)
        {
            switch(*args++)
            {
            case 'w': /* word */
                printf( "\tpushw %d(%%ebx)\n", pos );
                break;
            case 'l': /* long */
                printf( "\tpushl %d(%%ebx)\n", pos );
                break;
            }
            pos += 4;
        }
    }

    /* Push the return address */

    printf( "\tpushl " PREFIX "CALLTO16_RetAddr_%s\n",
            short_ret ? "word" : "long" );

    /* Push the called routine address */

    printf( "\tpushl 12(%%ebx)\n" );

    /* Get the 16-bit ds */

    if (reg_func)
    {
        printf( "\tpushw 16(%%ebx)\n" );
        printf( "\tmovl 32(%%ebx),%%ebx\n" ); /*Get ebx from the 32-bit stack*/
        printf( "\tpopw %%ds\n" );
    }
    else
    {
        /* Set ax equal to ds for window procedures */
        printf( "\tmovw 16(%%ebx),%%ax\n" );
#ifdef __svr4__
        printf( "\tdata16\n");
#endif
        printf( "\tmovw %%ax,%%ds\n" );
    }

    /* Jump to the called routine */

    printf( "\t.byte 0x66\n" );
    printf( "\tlret\n" );
}


/*******************************************************************
 *         BuildRet16Func
 *
 * Build the return code for 16-bit callbacks
 */
static void BuildRet16Func()
{
    printf( "\t.globl " PREFIX "CALLTO16_Ret_word\n" );
    printf( "\t.globl " PREFIX "CALLTO16_Ret_long\n" );

    /* Put return value into eax */

    printf( PREFIX "CALLTO16_Ret_long:\n" );
    printf( "\tpushw %%dx\n" );
    printf( "\tpushw %%ax\n" );
    printf( "\tpopl %%eax\n" );
    printf( PREFIX "CALLTO16_Ret_word:\n" );

    /* Restore 32-bit segment registers */

    printf( "\tmovw $0x%04x,%%bx\n", WINE_DATA_SELECTOR );
#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tmovw %%bx,%%ds\n" );
#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tmovw %%bx,%%es\n" );
#ifdef __svr4__
    printf( "\tdata16\n");
#endif
    printf( "\tmovw %%bx,%%ss\n" );

    /* Restore the 32-bit stack */

    printf( "\tmovl " PREFIX "IF1632_Saved32_esp,%%esp\n" );
    printf( "\tpopl " PREFIX "IF1632_Saved32_esp\n" );

    /* Restore the 32-bit registers */

    printf( "\tpopl %%edi\n" );
    printf( "\tpopl %%esi\n" );
    printf( "\tpopl %%edx\n" );
    printf( "\tpopl %%ecx\n" );
    printf( "\tpopl %%ebx\n" );

    /* Return to caller */

    printf( "\tpopl %%ebp\n" );
    printf( "\tlret\n" );

    /* Declare the return address variables */

    printf( "\t.data\n" );
    printf( "\t.globl " PREFIX "CALLTO16_RetAddr_word\n" );
    printf( "\t.globl " PREFIX "CALLTO16_RetAddr_long\n" );
    printf( PREFIX "CALLTO16_RetAddr_word:\t.long 0\n" );
    printf( PREFIX "CALLTO16_RetAddr_long:\t.long 0\n" );
    printf( "\t.text\n" );
}


/*******************************************************************
 *         BuildCallFrom32Func
 *
 * Build a 32-bit-to-Wine call-back function.
 * 'args' is the number of dword arguments.
 *
 * Stack layout:
 *   ...     ...
 * (ebp+12)  arg2
 * (ebp+8)   arg1
 * (ebp+4)   ret addr
 * (ebp)     ebp
 * (ebp-4)   func name
 * (ebp-8)   entry point
 */
static void BuildCallFrom32Func( int args )
{
    /* Function header */

    printf( "/**********\n" );
    printf( " * " PREFIX "CallFrom32_%d\n", args );
    printf( " **********/\n" );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "CallFrom32_%d\n\n", args );
    printf( PREFIX "CallFrom32_%d:\n", args );

    /* Entry code */

    printf( "\tleal 8(%%esp),%%ebp\n" );

    /* Print the debugging info */

    if (debugging)
    {
        printf( "\tpushl $%d\n", args );
        printf( "\tcall " PREFIX "RELAY_DebugCallFrom32\n" );
        printf( "\tadd $4, %%esp\n" );
    }

    /* Transfer the arguments */

    if (args)
    {
        int i;
        for (i = args; i > 0; i--) printf( "\tpushl %d(%%ebp)\n", 4 * i + 4 );
    }
    else
    {
        /* Push the address of the arguments. The called function will */
        /* ignore this if it really takes no arguments. */
        printf( "\tleal 8(%%ebp),%%eax\n" );
        printf( "\tpushl %%eax\n" );
    }

    /* Call the function */

    printf( "\tcall -8(%%ebp)\n" );

    /* Print the debugging info */

    if (debugging)
    {
        printf( "\tadd $%d,%%esp\n", args ? (args * 4) : 4 );
        printf( "\tpushl %%eax\n" );
        printf( "\tcall " PREFIX "RELAY_DebugCallFrom32Ret\n" );
        printf( "\tpopl %%eax\n" );
    }

    printf( "\tmovl %%ebp,%%esp\n" );
    printf( "\tpopl %%ebp\n" );

    /* Return, removing arguments */

    if (args) printf( "\tret $%d\n", args * 4 );
    else printf( "\tret\n" );
}


/*******************************************************************
 *         BuildCallTo32Func
 *
 * Build a Wine-to-32-bit callback function.
 *
 * Stack frame of the callback function:
 *  ...      ...
 * (ebp+16) arg2
 * (ebp+12) arg1
 * (ebp+8)  func to call
 * (ebp+4)  return address
 * (ebp)    previous ebp
 *
 * Prototype for the CallTo32 functions:
 *   extern LONG CallTo32_nn( FARPROC func, args... );
 */
static void BuildCallTo32Func( int args )
{
    /* Function header */

    printf( "/**********\n" );
    printf( " * " PREFIX "CallTo32_%d\n", args );
    printf( " **********/\n" );
    printf( "\t.align 4\n" );
    printf( "\t.globl " PREFIX "CallTo32_%d\n\n", args );
    printf( PREFIX "CallTo32_%d:\n", args );

    /* Entry code */

    printf( "\tpushl %%ebp\n" );
    printf( "\tmovl %%esp,%%ebp\n" );

    /* Transfer arguments */

    if (args)
    {
        int i;
        for (i = args; i > 0; i--) printf( "\tpushl %d(%%ebp)\n", 4 * i + 8 );
    }

    /* Print the debugging output */

    if (debugging)
    {
        printf( "\tpushl $%d\n", args );
        printf( "\tpushl 8(%%ebp)\n" );
        printf( "\tcall " PREFIX "RELAY_DebugCallTo32\n" );
        printf( "\taddl $8,%%esp\n" );
    }

    /* Call the function */

    printf( "\tcall 8(%%ebp)\n" );

    /* Return to Wine */

    printf( "\tmovl %%ebp,%%esp\n" );
    printf( "\tpopl %%ebp\n" );
    printf( "\tret\n" );
}


static void usage(void)
{
    fprintf(stderr, "usage: build -spec SPECNAMES\n"
                    "       build -callfrom16 FUNCTION_PROFILES\n"
                    "       build -callto16 FUNCTION_PROFILES\n"
                    "       build -callfrom32 FUNCTION_PROFILES\n"
                    "       build -callto32 FUNCTION_PROFILES\n" );
    exit(1);
}


int main(int argc, char **argv)
{
    int i;

    if (argc <= 2) usage();

    if (!strcmp( argv[1], "-spec" ))
    {
        for (i = 2; i < argc; i++) BuildSpecFiles( argv[i] );
    }
    else if (!strcmp( argv[1], "-callfrom16" ))  /* 16-bit-to-Wine callbacks */
    {
        /* File header */

        printf( "/* File generated automatically. Do not edit! */\n\n" );
        printf( "\t.text\n" );

        /* Build the 32-bit large stack callback */

        BuildCall32LargeStack();

        /* Build the callback functions */

        for (i = 2; i < argc; i++) BuildCallFrom16Func( argv[i] );

        /* Output the argument debugging strings */

        if (debugging)
        {
            printf( "/* Argument strings */\n" );
            for (i = 2; i < argc; i++)
            {
                printf( "Profile_%s:\n", argv[i] );
                printf( "\t.ascii \"%s\\0\"\n", argv[i] + 5 );
            }
        }
    }
    else if (!strcmp( argv[1], "-callto16" ))  /* Wine-to-16-bit callbacks */
    {
        /* File header */

        printf( "/* File generated automatically. Do not edit! */\n\n" );
        printf( "\t.text\n" );
        printf( "\t.globl " PREFIX "CALLTO16_Start\n" );
        printf( PREFIX "CALLTO16_Start:\n" );

        /* Build the callback functions */

        for (i = 2; i < argc; i++) BuildCallTo16Func( argv[i] );

        /* Output the 16-bit return code */

        BuildRet16Func();

        printf( "\t.globl " PREFIX "CALLTO16_End\n" );
        printf( PREFIX "CALLTO16_End:\n" );
    }
    else if (!strcmp( argv[1], "-callfrom32" ))  /* 32-bit-to-Wine callbacks */
    {
        /* File header */

        printf( "/* File generated automatically. Do not edit! */\n\n" );
        printf( "\t.text\n" );

        /* Build the callback functions */

        for (i = 2; i < argc; i++) BuildCallFrom32Func( atoi(argv[i]) );
    }
    else if (!strcmp( argv[1], "-callto32" ))  /* Wine-to-32-bit callbacks */
    {
        /* File header */

        printf( "/* File generated automatically. Do not edit! */\n\n" );
        printf( "\t.text\n" );

        /* Build the callback functions */

        for (i = 2; i < argc; i++) BuildCallTo32Func( atoi(argv[i]) );
    }
    else usage();

    return 0;
}

#endif  /* WINELIB */

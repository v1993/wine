/*
 * 	Registry Functions
 *
 * Copyright 1996 Marcus Meissner
 * Copyright 1998 Matthew Becker
 * Copyright 1999 Sylvain St-Germain
 *
 * December 21, 1997 - Kevin Cozens
 * Fixed bugs in the _w95_loadreg() function. Added extra information
 * regarding the format of the Windows '95 registry files.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * TODO
 *    Security access
 *    Option handling
 *    Time for RegEnumKey*, RegQueryInfoKey*
 */

#include "config.h"
#include "wine/port.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_LINUX_HDREG_H
# include <linux/hdreg.h>
#endif

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winioctl.h"
#include "ntddscsi.h"

#include "wine/library.h"
#include "wine/server.h"
#include "wine/unicode.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(reg);

#define SAVE_GLOBAL_REGBRANCH_USER_DEFAULT  "/wine.userreg"
#define SAVE_GLOBAL_REGBRANCH_LOCAL_MACHINE "/wine.systemreg"

#define MAX_PATHNAME_LEN   1024

#define IS_OPTION_FALSE(ch) \
    ((ch) == 'n' || (ch) == 'N' || (ch) == 'f' || (ch) == 'F' || (ch) == '0')



/******************************************************************************
 * _allocate_default_keys [Internal]
 * Registry initialisation, allocates some default keys.
 */
static void _allocate_default_keys(void)
{
    static const WCHAR StatDataW[] = {'D','y','n','D','a','t','a','\\',
                                      'P','e','r','f','S','t','a','t','s','\\',
                                      'S','t','a','t','D','a','t','a',0};
    static const WCHAR ConfigManagerW[] = {'D','y','n','D','a','t','a','\\',
                                           'C','o','n','f','i','g',' ','M','a','n','a','g','e','r','\\',
                                            'E','n','u','m',0};
    static const WCHAR Clone[] = {'M','a','c','h','i','n','e','\\',
                                  'S','y','s','t','e','m','\\',
                                  'C','l','o','n','e',0};
    HKEY hkey;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;

    TRACE("(void)\n");

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    RtlInitUnicodeString( &nameW, StatDataW );
    if (!NtCreateKey( &hkey, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL )) NtClose( hkey );

    RtlInitUnicodeString( &nameW, ConfigManagerW );
    if (!NtCreateKey( &hkey, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL )) NtClose( hkey );

    /* this key is generated when the nt-core booted successfully */
    RtlInitUnicodeString( &nameW, Clone );
    if (!NtCreateKey( &hkey, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL )) NtClose( hkey );
}



/* load the registry file in wine format [Internal] */
static void load_wine_registry(HKEY hkey,LPCSTR fn)
{
    WCHAR *buffer;
    HANDLE file;
    DWORD len;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;

    len = MultiByteToWideChar( CP_UNIXCP, 0, fn, -1, NULL, 0 );
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) ))) return;
    MultiByteToWideChar( CP_UNIXCP, 0, fn, -1, buffer, len );
    RtlInitUnicodeString( &name, buffer );

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = 0;
    attr.ObjectName = &name;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if (!NtOpenFile( &file, GENERIC_READ, &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT ))
    {
        SERVER_START_REQ( load_registry )
        {
            req->hkey    = hkey;
            req->file    = file;
            wine_server_call( req );
        }
        SERVER_END_REQ;
        CloseHandle( file );
    }
    HeapFree( GetProcessHeap(), 0, buffer );
}


/******************************************************************
 *		init_cdrom_registry
 *
 * Initializes registry to contain scsi info about the cdrom in NT.
 * All devices (even not real scsi ones) have this info in NT.
 * TODO: for now it only works for non scsi devices
 * NOTE: programs usually read these registry entries after sending the
 *       IOCTL_SCSI_GET_ADDRESS ioctl to the cdrom
 */
static void init_cdrom_registry( HANDLE handle )
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    WCHAR dataW[50];
    DWORD lenW;
    char buffer[40];
    DWORD value;
    const char *data;
    HKEY scsiKey;
    HKEY portKey;
    HKEY busKey;
    HKEY targetKey;
    DWORD disp;
    IO_STATUS_BLOCK io;
    SCSI_ADDRESS scsi_addr;

    if (NtDeviceIoControlFile( handle, 0, NULL, NULL, &io, IOCTL_SCSI_GET_ADDRESS,
                               NULL, 0, &scsi_addr, sizeof(scsi_addr) ))
        return;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    /* Ensure there is Scsi key */
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Machine\\HARDWARE\\DEVICEMAP\\Scsi" ) ||
        NtCreateKey( &scsiKey, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create DEVICEMAP\\Scsi registry key\n" );
        return;
    }
    RtlFreeUnicodeString( &nameW );

    snprintf(buffer,sizeof(buffer),"Scsi Port %d",scsi_addr.PortNumber);
    attr.RootDirectory = scsiKey;
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, buffer ) ||
        NtCreateKey( &portKey, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create DEVICEMAP\\Scsi Port registry key\n" );
        return;
    }
    RtlFreeUnicodeString( &nameW );

    RtlCreateUnicodeStringFromAsciiz( &nameW, "Driver" );
    data = "atapi";
    RtlMultiByteToUnicodeN( dataW, 50, &lenW, data, strlen(data));
    NtSetValueKey( portKey, &nameW, 0, REG_SZ, (BYTE*)dataW, lenW );
    RtlFreeUnicodeString( &nameW );
    value = 10;
    RtlCreateUnicodeStringFromAsciiz( &nameW, "FirstBusTimeScanInMs" );
    NtSetValueKey( portKey,&nameW, 0, REG_DWORD, (BYTE *)&value, sizeof(DWORD));
    RtlFreeUnicodeString( &nameW );
    value = 0;
#ifdef HDIO_GET_DMA
    {
        int fd, dma;
        if (!wine_server_handle_to_fd( handle, 0, &fd, NULL ))
        {
            if (ioctl(fd,HDIO_GET_DMA, &dma) != -1) value = dma;
            wine_server_release_fd( handle, fd );
        }
    }
#endif
    RtlCreateUnicodeStringFromAsciiz( &nameW, "DMAEnabled" );
    NtSetValueKey( portKey,&nameW, 0, REG_DWORD, (BYTE *)&value, sizeof(DWORD));
    RtlFreeUnicodeString( &nameW );

    snprintf(buffer,40,"Scsi Bus %d", scsi_addr.PathId);
    attr.RootDirectory = portKey;
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, buffer ) ||
        NtCreateKey( &busKey, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create DEVICEMAP\\Scsi Port\\Scsi Bus registry key\n" );
        return;
    }
    RtlFreeUnicodeString( &nameW );

    attr.RootDirectory = busKey;
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Initiator Id 255" ) ||
        NtCreateKey( &targetKey, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create DEVICEMAP\\Scsi Port\\Scsi Bus\\Initiator Id 255 registry key\n" );
        return;
    }
    RtlFreeUnicodeString( &nameW );
    NtClose( targetKey );

    snprintf(buffer,40,"Target Id %d", scsi_addr.TargetId);
    attr.RootDirectory = busKey;
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, buffer ) ||
        NtCreateKey( &targetKey, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create DEVICEMAP\\Scsi Port\\Scsi Bus 0\\Target Id registry key\n" );
        return;
    }
    RtlFreeUnicodeString( &nameW );

    RtlCreateUnicodeStringFromAsciiz( &nameW, "Type" );
    data = "CdRomPeripheral";
    RtlMultiByteToUnicodeN( dataW, 50, &lenW, data, strlen(data));
    NtSetValueKey( targetKey, &nameW, 0, REG_SZ, (BYTE*)dataW, lenW );
    RtlFreeUnicodeString( &nameW );
    /* FIXME - maybe read the real identifier?? */
    RtlCreateUnicodeStringFromAsciiz( &nameW, "Identifier" );
    data = "Wine CDROM";
    RtlMultiByteToUnicodeN( dataW, 50, &lenW, data, strlen(data));
    NtSetValueKey( targetKey, &nameW, 0, REG_SZ, (BYTE*)dataW, lenW );
    RtlFreeUnicodeString( &nameW );
    /* FIXME - we always use Cdrom0 - do not know about the nt behaviour */
    RtlCreateUnicodeStringFromAsciiz( &nameW, "DeviceName" );
    data = "Cdrom0";
    RtlMultiByteToUnicodeN( dataW, 50, &lenW, data, strlen(data));
    NtSetValueKey( targetKey, &nameW, 0, REG_SZ, (BYTE*)dataW, lenW );
    RtlFreeUnicodeString( &nameW );

    NtClose( targetKey );
    NtClose( busKey );
    NtClose( portKey );
    NtClose( scsiKey );
}


/* create the hardware registry branch */
static void create_hardware_branch(void)
{
    int i;
    HANDLE handle;
    char drive[] = "\\\\.\\A:";

    /* create entries for cdroms */
    for (i = 0; i < 26; i++)
    {
        drive[4] = 'A' + i;
        handle = CreateFileA( drive, 0, 0, NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) continue;
        init_cdrom_registry( handle );
        CloseHandle( handle );
    }
}


/* convert the drive type entries from the old format to the new one */
static void convert_drive_types(void)
{
    static const WCHAR TypeW[] = {'T','y','p','e',0};
    static const WCHAR drive_types_keyW[] = {'M','a','c','h','i','n','e','\\',
                                             'S','o','f','t','w','a','r','e','\\',
                                             'W','i','n','e','\\',
                                             'D','r','i','v','e','s',0 };
    WCHAR driveW[] = {'M','a','c','h','i','n','e','\\','S','o','f','t','w','a','r','e','\\',
                      'W','i','n','e','\\','W','i','n','e','\\',
                      'C','o','n','f','i','g','\\','D','r','i','v','e',' ','A',0};
    char tmp[32*sizeof(WCHAR) + sizeof(KEY_VALUE_PARTIAL_INFORMATION)];
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    DWORD dummy;
    ULONG disp;
    HKEY hkey_old, hkey_new;
    int i;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &nameW, drive_types_keyW );

    if (NtCreateKey( &hkey_new, KEY_ALL_ACCESS, &attr, 0, NULL, 0, &disp )) return;
    if (disp != REG_CREATED_NEW_KEY)
    {
        NtClose( hkey_new );
        return;
    }

    for (i = 0; i < 26; i++)
    {
        RtlInitUnicodeString( &nameW, driveW );
        nameW.Buffer[(nameW.Length / sizeof(WCHAR)) - 1] = 'A' + i;
        if (NtOpenKey( &hkey_old, KEY_ALL_ACCESS, &attr ) != STATUS_SUCCESS) continue;
        RtlInitUnicodeString( &nameW, TypeW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &dummy ))
        {
            WCHAR valueW[] = {'A',':',0};
            WCHAR *type = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;

            valueW[0] = 'A' + i;
            RtlInitUnicodeString( &nameW, valueW );
            NtSetValueKey( hkey_new, &nameW, 0, REG_SZ, type, (strlenW(type) + 1) * sizeof(WCHAR) );
            MESSAGE( "Converted drive type to new entry HKLM\\Software\\Wine\\Drives \"%c:\" = %s\n",
                     'A' + i, debugstr_w(type) );
        }
        NtClose( hkey_old );
    }
    NtClose( hkey_new );
}


/* convert the environment variable entries from the old format to the new one */
static void convert_environment( HKEY hkey_current_user )
{
    static const WCHAR wineW[] = {'M','a','c','h','i','n','e','\\',
                                  'S','o','f','t','w','a','r','e','\\',
                                  'W','i','n','e','\\','W','i','n','e','\\',
                                  'C','o','n','f','i','g','\\','W','i','n','e',0};
    static const WCHAR windowsW[] = {'w','i','n','d','o','w','s',0};
    static const WCHAR systemW[] = {'s','y','s','t','e','m',0};
    static const WCHAR windirW[] = {'w','i','n','d','i','r',0};
    static const WCHAR systemrootW[] = {'S','y','s','t','e','m','r','o','o','t',0};
    static const WCHAR winsysdirW[] = {'w','i','n','s','y','s','d','i','r',0};
    static const WCHAR envW[] = {'E','n','v','i','r','o','n','m','e','n','t',0};
    static const WCHAR tempW[] = {'T','E','M','P',0};
    static const WCHAR tmpW[] = {'T','M','P',0};
    static const WCHAR pathW[] = {'P','A','T','H',0};
    static const WCHAR profileW[] = {'p','r','o','f','i','l','e',0};
    static const WCHAR userprofileW[] = {'U','S','E','R','P','R','O','F','I','L','E',0};

    char buffer[1024*sizeof(WCHAR) + sizeof(KEY_VALUE_PARTIAL_INFORMATION)];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    DWORD dummy;
    ULONG disp;
    HKEY hkey_old, hkey_env;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &nameW, wineW );

    if (NtOpenKey( &hkey_old, KEY_ALL_ACCESS, &attr ) != STATUS_SUCCESS) return;

    attr.RootDirectory = hkey_current_user;
    RtlInitUnicodeString( &nameW, envW );
    if (NtCreateKey( &hkey_env, KEY_ALL_ACCESS, &attr, 0, NULL, 0, &disp ))
    {
        NtClose( hkey_old );
        return;
    }

    /* Test for existence of TEMP */
    RtlInitUnicodeString( &nameW, tempW );
    if (NtQueryValueKey(hkey_env, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
    {
        /* convert TEMP */
        RtlInitUnicodeString( &nameW, tempW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
        {
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            RtlInitUnicodeString( &nameW, tmpW );
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            MESSAGE( "Converted temp dir to new entry HKCU\\Environment \"TEMP\" = %s\n",
                    debugstr_w( (WCHAR*)info->Data ) );
        }
    }

    /* Test for existence of PATH */
    RtlInitUnicodeString( &nameW, pathW );
    if (NtQueryValueKey(hkey_env, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
    {
        /* convert PATH */
        RtlInitUnicodeString( &nameW, pathW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
        {
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            MESSAGE( "Converted path dir to new entry HKCU\\Environment \"PATH\" = %s\n",
                    debugstr_w( (WCHAR*)info->Data ) );
        }
    }

    /* Test for existence of USERPROFILE */
    RtlInitUnicodeString( &nameW, userprofileW );
    if (NtQueryValueKey(hkey_env, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
    {
        /* convert USERPROFILE */
        RtlInitUnicodeString( &nameW, profileW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
        {
            RtlInitUnicodeString( &nameW, userprofileW );
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            MESSAGE( "Converted profile dir to new entry HKCU\\Environment \"USERPROFILE\" = %s\n",
                    debugstr_w( (WCHAR*)info->Data ) );
        }
    }

    /* Test for existence of windir */
    RtlInitUnicodeString( &nameW, windirW );
    if (NtQueryValueKey(hkey_env, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
    {
        /* convert windir */
        RtlInitUnicodeString( &nameW, windowsW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
        {
            RtlInitUnicodeString( &nameW, windirW );
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            RtlInitUnicodeString( &nameW, systemrootW );
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            MESSAGE( "Converted windows dir to new entry HKCU\\Environment \"windir\" = %s\n",
                    debugstr_w( (WCHAR*)info->Data ) );
        }
    }
        
    /* Test for existence of winsysdir */
    RtlInitUnicodeString( &nameW, winsysdirW );
    if (NtQueryValueKey(hkey_env, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
    {
        /* convert winsysdir */
        RtlInitUnicodeString( &nameW, systemW );
        if (!NtQueryValueKey( hkey_old, &nameW, KeyValuePartialInformation, buffer, sizeof(buffer), &dummy ))
        {
            RtlInitUnicodeString( &nameW, winsysdirW );
            NtSetValueKey( hkey_env, &nameW, 0, info->Type, info->Data, info->DataLength );
            MESSAGE( "Converted system dir to new entry HKCU\\Environment \"winsysdir\" = %s\n",
                    debugstr_w( (WCHAR*)info->Data ) );
        }
    }
    NtClose( hkey_old );
    NtClose( hkey_env );
}


/* load all registry (native and global and home) */
void SHELL_LoadRegistry( void )
{
    HKEY hkey_local_machine, hkey_users, hkey_users_default, hkey_current_user, hkey_config;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    DWORD count;
    ULONG dispos;
    BOOL res;
    int all, period;
    char tmp[1024];

    static const WCHAR MachineW[] = {'M','a','c','h','i','n','e',0};
    static const WCHAR UserW[] = {'U','s','e','r',0};
    static const WCHAR DefaultW[] = {'.','D','e','f','a','u','l','t',0};
    static const WCHAR RegistryW[] = {'M','a','c','h','i','n','e','\\',
                                      'S','o','f','t','w','a','r','e','\\',
                                      'W','i','n','e','\\',
                                      'W','i','n','e','\\',
                                      'C','o','n','f','i','g','\\',
                                      'R','e','g','i','s','t','r','y',0};
    static const WCHAR load_global_reg_filesW[] = {'L','o','a','d','G','l','o','b','a','l','R','e','g','i','s','t','r','y','F','i','l','e','s',0};
    static const WCHAR SaveOnlyUpdatedKeysW[] = {'S','a','v','e','O','n','l','y','U','p','d','a','t','e','d','K','e','y','s',0};
    static const WCHAR PeriodicSaveW[] = {'P','e','r','i','o','d','i','c','S','a','v','e',0};
    static const WCHAR GlobalRegistryDirW[] = {'G','l','o','b','a','l','R','e','g','i','s','t','r','y','D','i','r',0};

    TRACE("(void)\n");

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    RtlInitUnicodeString( &nameW, UserW );
    NtCreateKey( &hkey_users, KEY_ALL_ACCESS, &attr, 0, NULL, 0, &dispos );
    if (dispos == REG_OPENED_EXISTING_KEY)
    {
        /* someone else already loaded the registry */
        NtClose( hkey_users );
        return;
    }

    RtlInitUnicodeString( &nameW, MachineW );
    NtCreateKey( &hkey_local_machine, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL );

    attr.RootDirectory = hkey_users;
    RtlInitUnicodeString( &nameW, DefaultW );
    if (NtCreateKey( &hkey_users_default, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL ))
    {
        ERR("Cannot create HKEY_USERS/.Default\n" );
        ExitProcess(1);
    }
    RtlOpenCurrentUser( KEY_ALL_ACCESS, &hkey_current_user );

    _allocate_default_keys();

    attr.RootDirectory = 0;
    RtlInitUnicodeString( &nameW, RegistryW );
    if (NtOpenKey( &hkey_config, KEY_ALL_ACCESS, &attr )) hkey_config = 0;

    /* load global registry if required */

    res = TRUE;
    RtlInitUnicodeString( &nameW, load_global_reg_filesW );
    if (!NtQueryValueKey( hkey_config, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &count ))
    {
        WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
        res = !IS_OPTION_FALSE(str[0]);
    }
    if (res)
    {
        /* load global registry files (stored in /etc/wine) */
        char *p, configfile[MAX_PATHNAME_LEN];

        /* Override ETCDIR? */
        configfile[0] = 0;
        RtlInitUnicodeString( &nameW, GlobalRegistryDirW );
        if (!NtQueryValueKey( hkey_config, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &count ))
        {
            WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
            RtlUnicodeToMultiByteN( configfile, sizeof(configfile), NULL, 
                                    str, (strlenW(str) + 1) * sizeof(WCHAR));
        }
        if (configfile[0] != '/') strcpy(configfile, ETCDIR);

        TRACE("GlobalRegistryDir is '%s'.\n", configfile);

        /* Load the global HKU hive directly from sysconfdir */
        p = configfile + strlen(configfile);
        strcpy(p, SAVE_GLOBAL_REGBRANCH_USER_DEFAULT);
        load_wine_registry( hkey_users, configfile );

        /* Load the global machine defaults directly from sysconfdir */
        strcpy(p, SAVE_GLOBAL_REGBRANCH_LOCAL_MACHINE);
        load_wine_registry( hkey_local_machine, configfile );
    }

    /* setup registry saving */

    all = FALSE;
    RtlInitUnicodeString( &nameW, SaveOnlyUpdatedKeysW );
    if (!NtQueryValueKey( hkey_config, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &count ))
    {
        WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
        all = IS_OPTION_FALSE(str[0]);
    }

    period = 0;
    RtlInitUnicodeString( &nameW, PeriodicSaveW );
    if (!NtQueryValueKey( hkey_config, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &count ))
    {
        WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
        period = (int)strtolW(str, NULL, 10);
    }

    /* load home registry and set saving level (0 for saving everything,
     * 1 for saving only modified keys) */

    SERVER_START_REQ( load_user_registries )
    {
        req->hkey = hkey_current_user;
        req->saving = !all;
        req->period = period * 1000;
        wine_server_call( req );
    }
    SERVER_END_REQ;

    /* create hardware registry branch */

    create_hardware_branch();

    /* convert keys from config file to new registry format */

    convert_drive_types();
    convert_environment( hkey_current_user );

    NtClose(hkey_users_default);
    NtClose(hkey_current_user);
    NtClose(hkey_users);
    NtClose(hkey_local_machine);
    NtClose(hkey_config);
}

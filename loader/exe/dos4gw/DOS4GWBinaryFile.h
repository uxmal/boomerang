/*
 * Copyright (C) 2000, The University of Queensland
 * Copyright (C) 2001, Sun Microsystems, Inc
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/** \file DOS4GWBinaryFile.h
 * \brief This file contains the definition of the class DOS4GWBinaryFile.
*/

#pragma once

#include "BinaryFile.h"
#include <string>

/**
 * This file contains the definition of the DOS4GWBinaryFile class, and some
 * other definitions specific to the exe version of the BinaryFile object
 * At present, this loader supports the OS2 file format (also known as
 * the Linear eXecutable format) as much as I've found necessary to
 * inspect old DOS4GW apps.  This loader could also be used for decompiling
 * Win9x VxD files or, of course, OS2 binaries, but you're probably better off
 * making a specific loader for each of these.
 */

// Given a little endian value x, load its value assuming little endian order
// Note: must be able to take address of x
// Note: Unlike the LH macro in BinaryFile.h, the paraeter is not a pointer
#define LMMH(x)                                                                                                        \
    ((unsigned)((Byte *)(&x))[0] + ((unsigned)((Byte *)(&x))[1] << 8) + ((unsigned)((Byte *)(&x))[2] << 16) +          \
    ((unsigned)((Byte *)(&x))[3] << 24))
// With this one, x IS a pounsigneder
#define LMMH2(x)                                                                                                       \
    ((unsigned)((Byte *)(x))[0] + ((unsigned)((Byte *)(x))[1] << 8) + ((unsigned)((Byte *)(x))[2] << 16) +             \
    ((unsigned)((Byte *)(x))[3] << 24))
#define LMMHw(x) ((unsigned)((Byte *)(&x))[0] + ((unsigned)((Byte *)(&x))[1] << 8))

#ifdef __GNUC__
#define PACKED __attribute__((packed))
#else
// TODO: what to do with PACKED?
#define PACKED
#endif


typedef struct {/* exe file header, just the signature really */
    Byte sigLo; /* .EXE signature: 0x4D 0x5A     */
    Byte sigHi;
} Header;

typedef struct PACKED {
    Byte sigLo;
    Byte sigHi;
    Byte byteord;
    Byte wordord;
    DWord formatlvl;
    SWord cputype;
    SWord ostype;
    DWord modulever;
    DWord moduleflags;
    DWord modulenumpages;
    DWord eipobjectnum;
    DWord eip;
    DWord espobjectnum;
    DWord esp;
    DWord pagesize;
    DWord pageoffsetshift;
    DWord fixupsectionsize;
    DWord fixupsectionchksum;
    DWord loadersectionsize;
    DWord loadersectionchksum;
    DWord objtbloffset;
    DWord numobjsinmodule;
    DWord objpagetbloffset;
    DWord objiterpagesoffset;
    DWord resourcetbloffset;
    DWord numresourcetblentries;
    DWord residentnametbloffset;
    DWord entrytbloffset;
    DWord moduledirectivesoffset;
    DWord nummoduledirectives;
    DWord fixuppagetbloffset;
    DWord fixuprecordtbloffset;
    DWord importtbloffset;
    DWord numimportmoduleentries;
    DWord importproctbloffset;
    DWord perpagechksumoffset;
    DWord datapagesoffset;
    DWord numpreloadpages;
    DWord nonresnametbloffset;
    DWord nonresnametbllen;
    DWord nonresnametblchksum;
    DWord autodsobjectnum;
    DWord debuginfooffset;
    DWord debuginfolen;
    DWord numinstancepreload;
    DWord numinstancedemand;
    DWord heapsize;
} LXHeader;

typedef struct PACKED {
    DWord VirtualSize;
    DWord RelocBaseAddr;
    DWord ObjectFlags;
    DWord PageTblIdx;
    DWord NumPageTblEntries;
    DWord Reserved1;
} LXObject;

typedef struct PACKED {
    DWord pagedataoffset;
    SWord datasize;
    SWord flags;
} LXPage;

// this is correct for internal fixups only
typedef struct PACKED {
    unsigned char src;
    unsigned char flags;
    short srcoff;
    //    unsigned char object;         // these are now variable length
    //    unsigned short trgoff;
} LXFixup;

//#ifdef WIN32
#pragma pack(4)
//#endif

class DOS4GWBinaryFile : public QObject, public LoaderInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LoaderInterface_iid)
    Q_INTERFACES(LoaderInterface)
public:
    DOS4GWBinaryFile();
    ~DOS4GWBinaryFile();          // Destructor
    void Close() override;                 // Close file opened with Open()
    void UnLoad() override;                // Unload the image
    LOAD_FMT GetFormat() const override;   // Get format (i.e.
    // LOADFMT_DOS4GW)
    MACHINE getMachine() const override; // Get machine (i.e.
    // MACHINE_Pentium)
    QString getFilename() const override { return m_pFileName; }
    ADDRESS getImageBase() override;
    size_t getImageSize() override;

    ADDRESS GetMainEntryPoint() override;
    ADDRESS GetEntryPoint() override;
    DWord getDelta();

    //
    //        --        --        --        --        --        --        --        --        --
    //
    // Internal information
    // Dump headers, etc
    virtual bool DisplayDetails(const char *fileName, FILE *f = stdout) override;

    void initialize(IBoomerang *sys) override;
protected:
    int dos4gwRead2(short *ps) const; // Read 2 bytes from native addr
    int dos4gwRead4(int *pi) const;   // Read 4 bytes from native addr

    bool RealLoad(const QString &sName) override; // Load the file; pure virtual
private:
    bool PostLoad(void *handle) override; //!< Called after archive member loaded

    Header *m_pHeader;      // Pointer to header
    LXHeader *m_pLXHeader;  // Pointer to lx header
    LXObject *m_pLXObjects; // Pointer to lx objects
    LXPage *m_pLXPages;     // Pointer to lx pages
    int m_cbImage;          // Size of image
    // int        m_cReloc;                // Number of relocation entries
    // DWord*    m_pRelocTable;            // The relocation table
    char *base; // Beginning of the loaded image
    // Map from address of dynamic pointers to library procedure names:
    QString m_pFileName;
    class IBinarySymbolTable *Symbols;
    class IBinaryImage *Image;
};

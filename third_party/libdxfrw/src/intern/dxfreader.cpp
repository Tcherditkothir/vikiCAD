/******************************************************************************
**  libDXFrw - Library to read/write DXF files (ascii & binary)              **
**                                                                           **
**  Copyright (C) 2011-2015 José F. Soriano, rallazz@gmail.com               **
**                                                                           **
**  This library is free software, licensed under the terms of the GNU       **
**  General Public License as published by the Free Software Foundation,     **
**  either version 2 of the License, or (at your option) any later version.  **
**  You should have received a copy of the GNU General Public License        **
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.    **
******************************************************************************/

#include <cstdlib>
#include <fstream>
#include <string>
#include <sstream>
#include "dxfreader.h"
#include "drw_textcodec.h"
#include "drw_dbg.h"

bool dxfReader::readRec(int *codeData) {
//    std::string text;
    int code;

    if (!readCode(&code))
        return false;
    *codeData = code;

    if (code < 10)
        readString();
    else if (code < 60)
        readDouble();
    else if (code < 80)
        readInt16();
    else if (code > 89 && code < 100) //TODO this is an int 32b
        readInt32();
    else if (code == 100 || code == 102 || code == 105)
        readString();
    else if (code > 109 && code < 150) //skip not used at the v2012
        readDouble();
    else if (code > 159 && code < 170) //skip not used at the v2012
        readInt64();
    else if (code < 180)
        readInt16();
    else if (code > 209 && code < 240) //skip not used at the v2012
        readDouble();
    else if (code > 269 && code < 290) //skip not used at the v2012
        readInt16();
    else if (code < 300) //TODO this is a boolean indicator, int in Binary?
        readBool();
    else if (code < 310)
        readString();
    else if (code < 320)
        readBinary();
    else if (code < 370)
        readString();
    else if (code < 390)
        readInt16();
    else if (code < 400)
        readString();
    else if (code < 410)
        readInt16();
    else if (code < 420)
        readString();
    else if (code < 430) //TODO this is an int 32b
        readInt32();
    else if (code < 440)
        readString();
    else if (code < 450) //TODO this is an int 32b
        readInt32();
    else if (code < 460) //TODO this is long??
        readInt32();
    else if (code < 470) //TODO this is a floating point double precision??
        readDouble();
    else if (code < 481)
        readString();
    else if( 999 == code && m_bIgnoreComments) {
        readString();
        return readRec( codeData);
    }
    else if (code == 1004)
        readBinary();
    else if (code > 998 && code < 1009) //skip not used at the v2012
        readString();
    else if (code < 1060) //TODO this is a floating point double precision??
        readDouble();
    else if (code < 1071)
        readInt16();
    else if (code == 1071) //TODO this is an int 32b
        readInt32();
    else if (skip)
        //skip safely this dxf entry ( ok for ascii dxf)
        readString();
    else
        //break in binary files because the conduct is unpredictable
        return false;

    return (filestr->good());
}
int dxfReader::getHandleString(){
    int res;
#if defined(__APPLE__)
    int Succeeded = sscanf ( strData.c_str(), "%x", &res );
    if ( !Succeeded || Succeeded == EOF )
        res = 0;
#else
    std::istringstream Convert(strData);
    if ( !(Convert >> std::hex >>res) )
        res = 0;
#endif
    return res;
}

bool dxfReaderBinary::readCode(int *code) {
    unsigned short *int16p;
    char buffer[2];
    filestr->read(buffer,2);
    int16p = (unsigned short *) buffer;
//exist a 32bits int (code 90) with 2 bytes???
    if ((*code == 90) && (*int16p>2000)){
        DRW_DBG(*code); DRW_DBG(" de 16bits\n");
        filestr->seekg(-4, std::ios_base::cur);
        filestr->read(buffer,2);
        int16p = (unsigned short *) buffer;
    }
    *code = *int16p;
    DRW_DBG(*code); DRW_DBG("\n");

    return (filestr->good());
}

bool dxfReaderBinary::readString() {
    type = STRING;
    std::getline(*filestr, strData, '\0');
    DRW_DBG(strData); DRW_DBG("\n");
    return (filestr->good());
}

bool dxfReaderBinary::readString(std::string *text) {
    type = STRING;
    std::getline(*filestr, *text, '\0');
    DRW_DBG(*text); DRW_DBG("\n");
    return (filestr->good());
}

bool dxfReaderBinary::readBinary() {
    unsigned char chunklen {0};

    filestr->read( reinterpret_cast<char *>(&chunklen), 1);
    filestr->seekg( chunklen, std::ios_base::cur);
    DRW_DBG( chunklen); DRW_DBG( " byte(s) binary data bypassed\n");

    return (filestr->good());
}

bool dxfReaderBinary::readInt16() {
    type = INT32;
    char buffer[2];
    filestr->read(buffer,2);
    intData = (int)((buffer[1] << 8) | buffer[0]);
    DRW_DBG(intData); DRW_DBG("\n");
    return (filestr->good());
}

bool dxfReaderBinary::readInt32() {
    type = INT32;
    unsigned int *int32p;
    char buffer[4];
    filestr->read(buffer,4);
    int32p = (unsigned int *) buffer;
    intData = *int32p;
    DRW_DBG(intData); DRW_DBG("\n");
    return (filestr->good());
}

bool dxfReaderBinary::readInt64() {
    type = INT64;
    unsigned long long int *int64p; //64 bits integer pointer
    char buffer[8];
    filestr->read(buffer,8);
    int64p = (unsigned long long int *) buffer;
    int64 = *int64p;
    DRW_DBG(int64); DRW_DBG(" int64\n");
    return (filestr->good());
}

bool dxfReaderBinary::readDouble() {
    type = DOUBLE;
    double *result;
    char buffer[8];
    filestr->read(buffer,8);
    result = (double *) buffer;
    doubleData = *result;
    DRW_DBG(doubleData); DRW_DBG("\n");
    return (filestr->good());
}

//saved as int or add a bool member??
bool dxfReaderBinary::readBool() {
    char buffer[1];
    filestr->read(buffer,1);
    intData = (int)(buffer[0]);
    DRW_DBG(intData); DRW_DBG("\n");
    return (filestr->good());
}

namespace {
/* A DXF group-code line is (blanks)(optional sign)(digits)(blanks). Anything
   else — the hallmark of a dwg2dxf raw-newline value spill — is not a code. */
bool looksLikeGroupCode(const std::string &s) {
    bool hasDigit = false, started = false;
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\r')
            continue;
        if ((c == '-' || c == '+') && !started) { started = true; continue; }
        if (c >= '0' && c <= '9') { hasDigit = true; started = true; continue; }
        return false;
    }
    return hasDigit;
}
/* dwg2dxf (LibreDWG) wraps long string values at this many bytes, inserting a
   raw CR/LF mid-value; the spill-over line carries no group code. Measured
   constant across real files (a handful land at 255 when a 2-byte UTF-8 char
   straddles the boundary). Only segments at/over this width can be wrapped. */
const std::string::size_type kDwgWrapWidth = 254;
} // namespace

/* VikiCAD patch 0004 (revised). The original release SKIPPED non-numeric
   spill lines to stop a one-line parse shift from corrupting the whole file
   ("tomato bug"), but that DROPPED the spilled text — truncating MTEXT such as
   "Immeuble protégé …" to "Immeuble protég". This version re-joins the
   continuation byte-for-byte (the inserted newline removed) so no text is
   lost, gated on the wrap width so well-formed DXF is never touched: a value
   is only extended when its segment is at the wrap width AND the next line is
   not a group code (in valid DXF a value is always followed by a numeric
   code, so the join never triggers there). */

bool dxfReaderAscii::nextRawLine(std::string &out) {
    if (m_hasPending) {
        out.swap(m_pending);
        m_pending.clear();
        m_hasPending = false;
        return true;
    }
    if (!std::getline(*filestr, out))
        return false;
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return true;
}

void dxfReaderAscii::pushRawLine(std::string &s) {
    m_pending.swap(s);
    m_hasPending = true;
}

bool dxfReaderAscii::readCode(int *code) {
    std::string text;
    /* With readString absorbing continuations this should rarely fire, but a
       spill after a value shorter than the wrap width could still land here;
       skip non-numeric lines as a bounded backstop. */
    for (int guard = 0; guard < 64; ++guard) {
        if (!nextRawLine(text))
            return false;
        if (looksLikeGroupCode(text)) {
            *code = atoi(text.c_str());
            DRW_DBG(*code); DRW_DBG("\n");
            return true;
        }
    }
    return false;
}

bool dxfReaderAscii::readString(std::string *text) {
    type = STRING;
    if (!nextRawLine(*text))
        return false;
    std::string::size_type seg = text->size();
    while (seg >= kDwgWrapWidth) {
        std::string peek;
        if (!nextRawLine(peek))
            break; // EOF: keep what we have
        if (looksLikeGroupCode(peek)) {
            pushRawLine(peek);
            break;
        }
        *text += peek;     // undo the wrap: append with no separator
        seg = peek.size();  // a full segment may itself be wrapped again
    }
    return true;
}

bool dxfReaderAscii::readString() {
    type = STRING;
    if (!nextRawLine(strData))
        return false;
    std::string::size_type seg = strData.size();
    while (seg >= kDwgWrapWidth) {
        std::string peek;
        if (!nextRawLine(peek))
            break;
        if (looksLikeGroupCode(peek)) {
            pushRawLine(peek);
            break;
        }
        strData += peek;
        seg = peek.size();
    }
    DRW_DBG(strData); DRW_DBG("\n");
    return true;
}

bool dxfReaderAscii::readBinary() {
    return readString();
}

bool dxfReaderAscii::readInt16() {
    type = INT32;
    std::string text;
    if (readString(&text)){
        intData = atoi(text.c_str());
        DRW_DBG(intData); DRW_DBG("\n");
        return true;
    } else
        return false;
}

bool dxfReaderAscii::readInt32() {
    type = INT32;
    return readInt16();
}

bool dxfReaderAscii::readInt64() {
    type = INT64;
    return readInt16();
}

bool dxfReaderAscii::readDouble() {
    type = DOUBLE;
    std::string text;
    if (readString(&text)){
#if defined(__APPLE__)
        int succeeded=sscanf( & (text[0]), "%lg", &doubleData);
        if(succeeded != 1) {
            DRW_DBG("dxfReaderAscii::readDouble(): reading double error: ");
            DRW_DBG(text);
            DRW_DBG('\n');
        }
#else
        std::istringstream sd(text);
        sd >> doubleData;
        DRW_DBG(doubleData); DRW_DBG('\n');
#endif
        return true;
    } else
        return false;
}

//saved as int or add a bool member??
bool dxfReaderAscii::readBool() {
    type = BOOL;
    std::string text;
    if (readString(&text)){
        intData = atoi(text.c_str());
        DRW_DBG(intData); DRW_DBG("\n");
        return true;
    } else
        return false;
}


#include "mjpegwriter.hpp"
#include "opencv2/core/core.hpp"
//#include "opencv2/core/utility.hpp"
#include <vector>

//uncomment for real stuff
//#define WITH_NEON
#ifdef WITH_NEON
#include "arm_neon.h"
#endif

namespace cv
{
namespace mjpeg
{

#define fourCC(a,b,c,d)   ((int)((uchar(d)<<24) | (uchar(c)<<16) | (uchar(b)<<8) | uchar(a)))

static const int AVIH_STRH_SIZE = 56;
static const int STRF_SIZE = 40;
static const int AVI_DWFLAG = 0x00000910;
static const int AVI_DWSCALE = 1;
static const int AVI_DWQUALITY = -1;
static const int JUNK_SEEK = 4096;
static const int AVIIF_KEYFRAME = 0x10;
static const int MAX_BYTES_PER_SEC = 99999999;
static const int SUG_BUFFER_SIZE = 1048576;

static const unsigned bit_mask[] =
{
    0,
    0x00000001, 0x00000003, 0x00000007, 0x0000000F,
    0x0000001F, 0x0000003F, 0x0000007F, 0x000000FF,
    0x000001FF, 0x000003FF, 0x000007FF, 0x00000FFF,
    0x00001FFF, 0x00003FFF, 0x00007FFF, 0x0000FFFF,
    0x0001FFFF, 0x0003FFFF, 0x0007FFFF, 0x000FFFFF,
    0x001FFFFF, 0x003FFFFF, 0x007FFFFF, 0x00FFFFFF,
    0x01FFFFFF, 0x03FFFFFF, 0x07FFFFFF, 0x0FFFFFFF,
    0x1FFFFFFF, 0x3FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF
};

class BitStream
{
public:
    enum
    {
        DEFAULT_BLOCK_SIZE = (1 << 15),
        huff_val_shift = 20,
        huff_code_mask = (1 << huff_val_shift) - 1
    };

    BitStream()
    {
        m_buf.resize(DEFAULT_BLOCK_SIZE + 1024);
        m_start = &m_buf[0];
        m_end = m_start + DEFAULT_BLOCK_SIZE;
        m_is_opened = false;
        m_f = 0;
    }

    ~BitStream()
    {
        close();
    }

    bool open(const std::string& filename)
    {
        close();
        m_f = fopen(filename.c_str(), "wb");
        if( !m_f )
            return false;
        m_current = m_start;
        m_pos = 0;
        return true;
    }

    bool isOpened() const { return m_f != 0; }

    void close()
    {
        writeBlock();
        if( m_f )
            fclose(m_f);
        m_f = 0;
    }

    void writeBlock()
    {
        size_t wsz0 = m_current - m_start;
        if( wsz0 > 0 && m_f )
        {
            size_t wsz = fwrite(m_start, 1, wsz0, m_f);
            CV_Assert( wsz == wsz0 );
        }
        m_pos += wsz0;
        m_current = m_start;
    }

    size_t getPos() const
    {
        return (size_t)(m_current - m_start) + m_pos;
    }

    void putByte(int val)
    {
        *m_current++ = (uchar)val;
        if( m_current >= m_end )
            writeBlock();
    }

    void putBytes(const uchar* buf, int count)
    {
        uchar* data = (uchar*)buf;
        CV_Assert(m_f && data && m_current && count >= 0);
        if( m_current >= m_end )
            writeBlock();

        while( count )
        {
            int l = (int)(m_end - m_current);

            if (l > count)
                l = count;

            if( l > 0 )
            {
                memcpy(m_current, data, l);
                m_current += l;
                data += l;
                count -= l;
            }
            if( m_current >= m_end )
                writeBlock();
        }
    }

    void putShort(int val)
    {
        m_current[0] = (uchar)val;
        m_current[1] = (uchar)(val >> 8);
        m_current += 2;
        if( m_current >= m_end )
            writeBlock();
    }

    void putInt(int val)
    {
        m_current[0] = (uchar)val;
        m_current[1] = (uchar)(val >> 8);
        m_current[2] = (uchar)(val >> 16);
        m_current[3] = (uchar)(val >> 24);
        m_current += 4;
        if( m_current >= m_end )
            writeBlock();
    }

    void jputShort(int val)
    {
        m_current[0] = (uchar)(val >> 8);
        m_current[1] = (uchar)val;
        m_current += 2;
        if( m_current >= m_end )
            writeBlock();
    }

    void patchInt(int val, size_t pos)
    {
        if( pos >= m_pos )
        {
            size_t delta = pos - m_pos;
            CV_Assert( delta < m_current - m_start );
            m_start[delta] = (uchar)val;
            m_start[delta+1] = (uchar)(val >> 8);
            m_start[delta+2] = (uchar)(val >> 16);
            m_start[delta+3] = (uchar)(val >> 24);
        }
        else
        {
            size_t fpos = ftell(m_f);
            fseek(m_f, pos, SEEK_SET);
            uchar buf[] = { (uchar)val, (uchar)(val >> 8), (uchar)(val >> 16), (uchar)(val >> 24) };
            fwrite(buf, 1, 4, m_f);
            fseek(m_f, fpos, SEEK_SET);
        }
    }

    void jput(unsigned currval)
    {
        uchar v;
        uchar* ptr = m_current;
        v = (uchar)(currval >> 24);
        *ptr++ = v;
        if( v == 255 )
            *ptr++ = 0;
        v = (uchar)(currval >> 16);
        *ptr++ = v;
        if( v == 255 )
            *ptr++ = 0;
        v = (uchar)(currval >> 8);
        *ptr++ = v;
        if( v == 255 )
            *ptr++ = 0;
        v = (uchar)currval;
        *ptr++ = v;
        if( v == 255 )
            *ptr++ = 0;
        m_current = ptr;
        if( m_current >= m_end )
            writeBlock();
    }

    static bool createEncodeHuffmanTable( const int* src, unsigned* table, int max_size )
    {
        int  i, k;
        int  min_val = INT_MAX, max_val = INT_MIN;
        int  size;

        /* calc min and max values in the table */
        for( i = 1, k = 1; src[k] >= 0; i++ )
        {
            int code_count = src[k++];

            for( code_count += k; k < code_count; k++ )
            {
                int  val = src[k] >> huff_val_shift;
                if( val < min_val )
                    min_val = val;
                if( val > max_val )
                    max_val = val;
            }
        }

        size = max_val - min_val + 3;

        if( size > max_size )
        {
            CV_Error(CV_StsOutOfRange, "too big maximum Huffman code size");
            return false;
        }

        memset( table, 0, size*sizeof(table[0]));

        table[0] = min_val;
        table[1] = size - 2;

        for( i = 1, k = 1; src[k] >= 0; i++ )
        {
            int code_count = src[k++];

            for( code_count += k; k < code_count; k++ )
            {
                int  val = src[k] >> huff_val_shift;
                int  code = src[k] & huff_code_mask;
                
                table[val - min_val + 2] = (code << 8) | i;
            }
        }
        return true;
    }

    static int* createSourceHuffmanTable(const uchar* src, int* dst,
                                         int max_bits, int first_bits)
    {
        int   i, val_idx, code = 0;
        int*  table = dst;
        *dst++ = first_bits;
        for (i = 1, val_idx = max_bits; i <= max_bits; i++)
        {
            int code_count = src[i - 1];
            dst[0] = code_count;
            code <<= 1;
            for (int k = 0; k < code_count; k++)
            {
                dst[k + 1] = (src[val_idx + k] << huff_val_shift) | (code + k);
            }
            code += code_count;
            dst += code_count + 1;
            val_idx += code_count;
        }
        dst[0] = -1;
        return  table;
    }

protected:
    std::vector<uchar> m_buf;
    uchar*  m_start;
    uchar*  m_end;
    uchar*  m_current;
    size_t  m_pos;
    bool    m_is_opened;
    FILE*   m_f;
};


MJpegWriter::~MJpegWriter() {}

class MJpegWriterImpl : public MJpegWriter
{
public:
    MJpegWriterImpl() { rawstream = false; }
    MJpegWriterImpl(const std::string& filename, Size size, double fps, int _colorspace)
    {
        rawstream = false;
        open(filename, size, fps, _colorspace);
    }
    ~MJpegWriterImpl() { close(); }

    void close()
    {
        if( !strm.isOpened() )
            return;

        if( !frameOffset.empty() && !rawstream )
        {
            endWriteChunk(); // end LIST 'movi'
            writeIndex();
            finishWriteAVI();
        }
        strm.close();
        frameOffset.clear();
        frameSize.clear();
        AVIChunkSizeIndex.clear();
        frameNumIndexes.clear();
    }

    bool open(const std::string& filename, Size size, double fps, int _colorspace)
    {
        close();
        bool ok = strm.open(filename);
        if( !ok )
            return false;

        CV_Assert(fps >= 1);
        outfps = cvRound(fps);
        width = size.width;
        height = size.height;
        quality = 3;
        rawstream = false;
        colorspace = _colorspace;
        channels = colorspace == COLORSPACE_GRAY ? 1 : 3;

        if( !rawstream )
        {
            startWriteAVI();
            writeStreamHeader();
        }
        return true;
    }

    bool isOpened() const { return strm.isOpened(); }

    void startWriteAVI()
    {
        startWriteChunk(fourCC('R', 'I', 'F', 'F'));

        strm.putInt(fourCC('A', 'V', 'I', ' '));

        startWriteChunk(fourCC('L', 'I', 'S', 'T'));

        strm.putInt(fourCC('h', 'd', 'r', 'l'));
        strm.putInt(fourCC('a', 'v', 'i', 'h'));
        strm.putInt(AVIH_STRH_SIZE);
        strm.putInt(cvRound(1e6 / outfps));
        strm.putInt(MAX_BYTES_PER_SEC);
        strm.putInt(0);
        strm.putInt(AVI_DWFLAG);

        frameNumIndexes.push_back(strm.getPos());

        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(1); // number of streams
        strm.putInt(SUG_BUFFER_SIZE);
        strm.putInt(width);
        strm.putInt(height);
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(0);
    }

    void writeStreamHeader()
    {
        // strh
        startWriteChunk(fourCC('L', 'I', 'S', 'T'));

        strm.putInt(fourCC('s', 't', 'r', 'l'));
        strm.putInt(fourCC('s', 't', 'r', 'h'));
        strm.putInt(AVIH_STRH_SIZE);
        strm.putInt(fourCC('v', 'i', 'd', 's'));
        strm.putInt(fourCC('M', 'J', 'P', 'G'));
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(AVI_DWSCALE);
        strm.putInt(outfps);
        strm.putInt(0);

        frameNumIndexes.push_back(strm.getPos());

        strm.putInt(0);
        strm.putInt(SUG_BUFFER_SIZE);
        strm.putInt(AVI_DWQUALITY);
        strm.putInt(0);
        strm.putShort(0);
        strm.putShort(0);
        strm.putShort(width);
        strm.putShort(height);

        // strf (use the BITMAPINFOHEADER for video)
        startWriteChunk(fourCC('s', 't', 'r', 'f'));

        strm.putInt(STRF_SIZE);
        strm.putInt(width);
        strm.putInt(height);
        strm.putShort(1); // planes (1 means interleaved data (after decompression))

        strm.putShort(channels); // bits per pixel
        strm.putInt(fourCC('M', 'J', 'P', 'G'));
        strm.putInt(width * height * channels);
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(0);
        strm.putInt(0);
        // Must be indx chunk
        endWriteChunk(); // end strf
        endWriteChunk(); // end strl

        // odml
        startWriteChunk(fourCC('L', 'I', 'S', 'T'));
        strm.putInt(fourCC('o', 'd', 'm', 'l'));
        startWriteChunk(fourCC('d', 'm', 'l', 'h'));

        frameNumIndexes.push_back(strm.getPos());

        strm.putInt(0);
        strm.putInt(0);

        endWriteChunk(); // end dmlh
        endWriteChunk(); // end odml

        endWriteChunk(); // end hdrl

        // JUNK
        startWriteChunk(fourCC('J', 'U', 'N', 'K'));
        size_t pos = strm.getPos();
        for( ; pos < (size_t)JUNK_SEEK; pos += 4 )
            strm.putInt(0);
        endWriteChunk(); // end JUNK
                         // movi
        startWriteChunk(fourCC('L', 'I', 'S', 'T'));
        moviPointer = strm.getPos();
        strm.putInt(fourCC('m', 'o', 'v', 'i'));
    }

    void startWriteChunk(int fourcc)
    {
        CV_Assert(fourcc != 0);
        strm.putInt(fourcc);

        AVIChunkSizeIndex.push_back(strm.getPos());
        strm.putInt(0);
    }

    void endWriteChunk()
    {
        if( !AVIChunkSizeIndex.empty() )
        {
            size_t currpos = strm.getPos();
            size_t pospos = AVIChunkSizeIndex.back();
            AVIChunkSizeIndex.pop_back();
            int chunksz = (int)(currpos - (pospos + 4));
            strm.patchInt(chunksz, pospos);
        }
    }

    void writeIndex()
    {
        // old style AVI index. Must be Open-DML index
        startWriteChunk(fourCC('i', 'd', 'x', '1'));
        int nframes = (int)frameOffset.size();
        for( int i = 0; i < nframes; i++ )
        {
            strm.putInt(fourCC('0', '0', 'd', 'c'));
            strm.putInt(AVIIF_KEYFRAME);
            strm.putInt((int)frameOffset[i]);
            strm.putInt((int)frameSize[i]);
        }
        endWriteChunk(); // End idx1
    }

    void finishWriteAVI()
    {
        int nframes = (int)frameOffset.size();
        // Record frames numbers to AVI Header
        while (!frameNumIndexes.empty())
        {
            size_t ppos = frameNumIndexes.back();
            frameNumIndexes.pop_back();
            strm.patchInt(nframes, ppos);
        }
        endWriteChunk(); // end RIFF
    }

    bool write(const Mat& img)
    {
        size_t chunkPointer = strm.getPos();
        int input_channels = img.channels();

        if( colorspace == COLORSPACE_GRAY )
        {
            CV_Assert( img.cols == width && img.rows == height && input_channels == 1 );
        }
        else if( colorspace == COLORSPACE_RGBA )
        {
            CV_Assert( img.cols == width && img.rows == height && input_channels == 4 );
        }
        else if( colorspace == COLORSPACE_BGR )
        {
            CV_Assert( img.cols == width && img.rows == height && input_channels == 3 );
        }
        else if( colorspace == COLORSPACE_YUV444P )
        {
            CV_Assert( img.cols == width && img.rows == height*3 && input_channels == 1 );
        }

        if( !rawstream )
            startWriteChunk(fourCC('0', '0', 'd', 'c'));

        writeFrameData(img.data, (int)img.step, width, height, input_channels);

        if( !rawstream )
        {
            frameOffset.push_back(chunkPointer - moviPointer);
            frameSize.push_back(strm.getPos() - chunkPointer - 8);       // Size excludes '00dc' and size field
            endWriteChunk(); // end '00dc'
        }

        return true;
    }

    void writeFrameData( const uchar* data, int step, int width, int height, int input_channels );

protected:
    int outfps;
    int width, height, channels;
    int quality;
    size_t moviPointer;
    std::vector<size_t> frameOffset, frameSize, AVIChunkSizeIndex, frameNumIndexes;
    int colorspace;
    bool rawstream;

    BitStream strm;
};

#define DCT_DESCALE(x, n) (((x) + (((int)1) << ((n) - 1))) >> (n))
#define fix(x, n)   (int)((x)*(1 << (n)) + .5);

enum
{
    fixb = 14,
    fixc = 12,
    postshift = 14
};

static const int C0_707 = fix(0.707106781f, fixb);
static const int C0_541 = fix(0.541196100f, fixb);
static const int C0_382 = fix(0.382683432f, fixb);
static const int C1_306 = fix(1.306562965f, fixb);

static const int y_r = fix(0.299, fixc);
static const int y_g = fix(0.587, fixc);
static const int y_b = fix(0.114, fixc);

static const int cb_r = -fix(0.1687, fixc);
static const int cb_g = -fix(0.3313, fixc);
static const int cb_b = fix(0.5, fixc);

static const int cr_r = fix(0.5, fixc);
static const int cr_g = -fix(0.4187, fixc);
static const int cr_b = -fix(0.0813, fixc);

// Standard JPEG quantization tables
static const uchar jpegTableK1_T[] =
{
    16, 12, 14, 14,  18,  24,  49,  72,
    11, 12, 13, 17,  22,  35,  64,  92,
    10, 14, 16, 22,  37,  55,  78,  95,
    16, 19, 24, 29,  56,  64,  87,  98,
    24, 26, 40, 51,  68,  81, 103, 112,
    40, 58, 57, 87, 109, 104, 121, 100,
    51, 60, 69, 80, 103, 113, 120, 103,
    61, 55, 56, 62,  77,  92, 101,  99
};

static const uchar jpegTableK2_T[] =
{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// Standard Huffman tables

// ... for luma DCs.
static const uchar jpegTableK3[] =
{
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

// ... for chroma DCs.
static const uchar jpegTableK4[] =
{
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

// ... for luma ACs.
static const uchar jpegTableK5[] =
{
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

// ... for chroma ACs
static const uchar jpegTableK6[] =
{
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const uchar zigzag[] =
{
    0,  8,  1,  2,  9, 16, 24, 17, 10,  3,  4, 11, 18, 25, 32, 40,
    33, 26, 19, 12,  5,  6, 13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14,  7, 15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};


static const int idct_prescale[] =
{
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
    8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
    4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247
};

static const char jpegHeader[] =
"\xFF\xD8"  // SOI  - start of image
"\xFF\xE0"  // APP0 - jfif extention
"\x00\x10"  // 2 bytes: length of APP0 segment
"JFIF\x00"  // JFIF signature
"\x01\x02"  // version of JFIF
"\x00"      // units = pixels ( 1 - inch, 2 - cm )
"\x00\x01\x00\x01" // 2 2-bytes values: x density & y density
"\x00\x00"; // width & height of thumbnail: ( 0x0 means no thumbnail)

#ifdef WITH_NEON
// FDCT with postscaling
static void aan_fdct8x8( const short *src, short *dst,
                         int step, const short *postscale )
{
    // Pass 1: process rows
    int16x8_t x0 = vld1q_s16(src);    int16x8_t x1 = vld1q_s16(src + step*7);
    int16x8_t x2 = vld1q_s16(src + step*3);    int16x8_t x3 = vld1q_s16(src + step*4);

    int16x8_t x4 = vaddq_s16(x0, x1);    x0 = vsubq_s16(x0, x1);
    x1 = vaddq_s16(x2, x3);    x2 = vsubq_s16(x2, x3);

    int16x8_t t1 = x0; int16x8_t t2 = x2;

    x2 = vaddq_s16(x4, x1);    x4 = vsubq_s16(x4, x1);

    x0 = vld1q_s16(src + step);    x3 = vld1q_s16(src + step*6);

    x1 = vaddq_s16(x0, x3);    x0 = vsubq_s16(x0, x3);
    int16x8_t t3 = x0;

    x0 = vld1q_s16(src + step*2);    x3 = vld1q_s16(src + step*5);

    int16x8_t t4 = vsubq_s16(x0, x3);

    x0 = vaddq_s16(x0, x3);
    x3 = vaddq_s16(x0, x1);    x0 = vsubq_s16(x0, x1);
    x1 = vaddq_s16(x2, x3);    x2 = vsubq_s16(x2, x3);

    int16x8_t res0 = x1;
    int16x8_t res4 = x2;
    x0 = vqdmulhq_n_s16(vsubq_s16(x0, x4), (short)(C0_707*2));
    x1 = vaddq_s16(x4, x0);    x4 = vsubq_s16(x4, x0);

    int16x8_t res2 = x4;
    int16x8_t res6 = x1;

    x0 = t2;    x1 = t4;
    x2 = t3;    x3 = t1;
    x0 = vaddq_s16(x0, x1);    x1 = vaddq_s16(x1, x2);    x2 = vaddq_s16(x2, x3);
    x1 =vqdmulhq_n_s16(x1, (short)(C0_707*2));

    x4 = vaddq_s16(x1, x3);    x3 = vsubq_s16(x3, x1);
    x1 = vqdmulhq_n_s16(vsubq_s16(x0, x2), (short)(C0_382*2));
    x0 = vaddq_s16(vqdmulhq_n_s16(x0, (short)(C0_541*2)), x1);
    x2 = vaddq_s16(vshlq_n_s16(vqdmulhq_n_s16(x2, (short)C1_306), 1), x1);

    x1 = vaddq_s16(x0, x3);    x3 = vsubq_s16(x3, x0);
    x0 = vaddq_s16(x4, x2);    x4 = vsubq_s16(x4, x2);

    int16x8_t res1 = x0;
    int16x8_t res3 = x3;
    int16x8_t res5 = x1;
    int16x8_t res7 = x4;

    //transpose a matrix
    /*
     res0 00 01 02 03 04 05 06 07
     res1 10 11 12 13 14 15 16 17
     res2 20 21 22 23 24 25 26 27
     res3 30 31 32 33 34 35 36 37
     res4 40 41 42 43 44 45 46 47
     res5 50 51 52 53 54 55 56 57
     res6 60 61 62 63 64 65 66 67
     res7 70 71 72 73 74 75 76 77
     */

    //transpose elements 00-33
    int16x4_t res0_0 = vget_low_s16(res0);
    int16x4_t res1_0 = vget_low_s16(res1);
    int16x4x2_t tres = vtrn_s16(res0_0, res1_0);
    int32x4_t l0 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    res0_0 = vget_low_s16(res2);
    res1_0 = vget_low_s16(res3);
    tres = vtrn_s16(res0_0, res1_0);
    int32x4_t l1 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    int32x4x2_t tres1 = vtrnq_s32(l0, l1);

    // transpose elements 40-73
    res0_0 = vget_low_s16(res4);
    res1_0 = vget_low_s16(res5);
    tres = vtrn_s16(res0_0, res1_0);
    l0 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    res0_0 = vget_low_s16(res6);
    res1_0 = vget_low_s16(res7);

    tres = vtrn_s16(res0_0, res1_0);
    l1 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    int32x4x2_t tres2 = vtrnq_s32(l0, l1);

    //combine into 0-3
    int16x8_t transp_res0 =  vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(tres1.val[0]), vget_low_s32(tres2.val[0])));
    int16x8_t transp_res1 =  vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(tres1.val[0]), vget_high_s32(tres2.val[0])));
    int16x8_t transp_res2 =  vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(tres1.val[1]), vget_low_s32(tres2.val[1])));
    int16x8_t transp_res3 =  vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(tres1.val[1]), vget_high_s32(tres2.val[1])));

    // transpose elements 04-37
    res0_0 = vget_high_s16(res0);
    res1_0 = vget_high_s16(res1);
    tres = vtrn_s16(res0_0, res1_0);
    l0 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    res0_0 = vget_high_s16(res2);
    res1_0 = vget_high_s16(res3);

    tres = vtrn_s16(res0_0, res1_0);
    l1 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    tres1 = vtrnq_s32(l0, l1);

    // transpose elements 44-77
    res0_0 = vget_high_s16(res4);
    res1_0 = vget_high_s16(res5);
    tres = vtrn_s16(res0_0, res1_0);
    l0 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    res0_0 = vget_high_s16(res6);
    res1_0 = vget_high_s16(res7);

    tres = vtrn_s16(res0_0, res1_0);
    l1 = vcombine_s32(vreinterpret_s32_s16(tres.val[0]),vreinterpret_s32_s16(tres.val[1]));

    tres2 = vtrnq_s32(l0, l1);

    //combine into 4-7
    int16x8_t transp_res4 =  vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(tres1.val[0]), vget_low_s32(tres2.val[0])));
    int16x8_t transp_res5 =  vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(tres1.val[0]), vget_high_s32(tres2.val[0])));
    int16x8_t transp_res6 =  vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(tres1.val[1]), vget_low_s32(tres2.val[1])));
    int16x8_t transp_res7 =  vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(tres1.val[1]), vget_high_s32(tres2.val[1])));

    //special hack for vqdmulhq_s16 command that is producing -1 instead of 0
#define STORE_DESCALED(addr, reg, mul_addr)            postscale_line = vld1q_s16((mul_addr)); \
            mask = vreinterpretq_s16_u16(vcltq_s16((reg), z)); \
            reg = vabsq_s16(reg); \
            reg = vqdmulhq_s16(vqaddq_s16((reg), (reg)), postscale_line); \
            reg = vsubq_s16(veorq_s16(reg, mask), mask); \
            vst1q_s16((addr), reg);

    int16x8_t z = vdupq_n_s16(0), postscale_line, mask;

    // pass 2: process columns
    x0 = transp_res0;    x1 = transp_res7;
    x2 = transp_res3;    x3 = transp_res4;

    x4 = vaddq_s16(x0, x1);   x0 = vsubq_s16(x0, x1);
    x1 = vaddq_s16(x2, x3);    x2 = vsubq_s16(x2, x3);

    t1 = x0; t2 = x2;

    x2 = vaddq_s16(x4, x1);    x4 = vsubq_s16(x4, x1);

    x0 = transp_res1;
    x3 = transp_res6;

    x1 = vaddq_s16(x0, x3);    x0 = vsubq_s16(x0, x3);

    t3 = x0;

    x0 = transp_res2; x3 = transp_res5;

    t4 = vsubq_s16(x0, x3);

    x0 = vaddq_s16(x0, x3);

    x3 = vaddq_s16(x0, x1);    x0 = vsubq_s16(x0, x1);
    x1 = vaddq_s16(x2, x3);    x2 = vsubq_s16(x2, x3);

    STORE_DESCALED(dst, x1, postscale);
    STORE_DESCALED(dst + 4*8, x2, postscale + 4*8);

    x0 = vqdmulhq_n_s16(vsubq_s16(x0, x4), (short)(C0_707*2));

    x1 = vaddq_s16(x4, x0);    x4 = vsubq_s16(x4, x0);

    STORE_DESCALED(dst + 2*8, x4,postscale + 2*8);
    STORE_DESCALED(dst + 6*8, x1,postscale + 6*8);

    x0 = t2; x1 = t4;
    x2 = t3; x3 = t1;

    x0 = vaddq_s16(x0, x1);    x1 = vaddq_s16(x1, x2);    x2 = vaddq_s16(x2, x3);

    x1 =vqdmulhq_n_s16(x1, (short)(C0_707*2));

    x4 = vaddq_s16(x1, x3);    x3 = vsubq_s16(x3, x1);

    x1 = vqdmulhq_n_s16(vsubq_s16(x0, x2), (short)(C0_382*2));
    x0 = vaddq_s16(vqdmulhq_n_s16(x0, (short)(C0_541*2)), x1);
    x2 = vaddq_s16(vshlq_n_s16(vqdmulhq_n_s16(x2, (short)C1_306), 1), x1);

    x1 = vaddq_s16(x0, x3);    x3 = vsubq_s16(x3, x0);
    x0 = vaddq_s16(x4, x2);    x4 = vsubq_s16(x4, x2);

    STORE_DESCALED(dst + 5*8, x1,postscale + 5*8);
    STORE_DESCALED(dst + 1*8, x0,postscale + 1*8);
    STORE_DESCALED(dst + 7*8, x4,postscale + 7*8);
    STORE_DESCALED(dst + 3*8, x3,postscale + 3*8);
}

#else
// FDCT with postscaling
static void aan_fdct8x8( const short *src, short *dst,
                         int step, const short *postscale )
{
    short  workspace[64], *work = workspace;
    int  i;

    // Pass 1: process rows
    for( i = 8; i > 0; i--, src += step, work += 8 )
    {
        int x0 = src[0], x1 = src[7];
        int x2 = src[3], x3 = src[4];

        int x4 = x0 + x1; x0 -= x1;
        x1 = x2 + x3; x2 -= x3;

        work[7] = x0; work[1] = x2;
        x2 = x4 + x1; x4 -= x1;

        x0 = src[1]; x3 = src[6];
        x1 = x0 + x3; x0 -= x3;
        work[5] = x0;

        x0 = src[2]; x3 = src[5];
        work[3] = x0 - x3; x0 += x3;

        x3 = x0 + x1; x0 -= x1;
        x1 = x2 + x3; x2 -= x3;

        work[0] = x1; work[4] = x2;

        x0 = DCT_DESCALE((x0 - x4)*C0_707, fixb);
        x1 = x4 + x0; x4 -= x0;
        work[2] = x4; work[6] = x1;

        x0 = work[1]; x1 = work[3];
        x2 = work[5]; x3 = work[7];

        x0 += x1; x1 += x2; x2 += x3;
        x1 = DCT_DESCALE(x1*C0_707, fixb);

        x4 = x1 + x3; x3 -= x1;
        x1 = (x0 - x2)*C0_382;
        x0 = DCT_DESCALE(x0*C0_541 + x1, fixb);
        x2 = DCT_DESCALE(x2*C1_306 + x1, fixb);

        x1 = x0 + x3; x3 -= x0;
        x0 = x4 + x2; x4 -= x2;

        work[5] = x1; work[1] = x0;
        work[7] = x4; work[3] = x3;
    }

    work = workspace;
    // pass 2: process columns
    for( i = 8; i > 0; i--, work++, postscale ++, dst += 8 )
    {
        int  x0 = work[8*0], x1 = work[8*7];
        int  x2 = work[8*3], x3 = work[8*4];

        int  x4 = x0 + x1; x0 -= x1;
        x1 = x2 + x3; x2 -= x3;

        work[8*7] = x0; work[8*0] = x2;
        x2 = x4 + x1; x4 -= x1;

        x0 = work[8*1]; x3 = work[8*6];
        x1 = x0 + x3; x0 -= x3;
        work[8*4] = x0;

        x0 = work[8*2]; x3 = work[8*5];
        work[8*3] = x0 - x3; x0 += x3;

        x3 = x0 + x1; x0 -= x1;
        x1 = x2 + x3; x2 -= x3;

        dst[0] = DCT_DESCALE(x1*postscale[0*8], postshift);
        dst[4] = DCT_DESCALE(x2*postscale[4*8], postshift);

        x0 = DCT_DESCALE((x0 - x4)*C0_707, fixb);
        x1 = x4 + x0; x4 -= x0;

        dst[2] = DCT_DESCALE(x4*postscale[2*8], postshift);
        dst[6] = DCT_DESCALE(x1*postscale[6*8], postshift);

        x0 = work[8*0]; x1 = work[8*3];
        x2 = work[8*4]; x3 = work[8*7];

        x0 += x1; x1 += x2; x2 += x3;
        x1 = DCT_DESCALE(x1*C0_707, fixb);

        x4 = x1 + x3; x3 -= x1;
        x1 = (x0 - x2)*C0_382;
        x0 = DCT_DESCALE(x0*C0_541 + x1, fixb);
        x2 = DCT_DESCALE(x2*C1_306 + x1, fixb);

        x1 = x0 + x3; x3 -= x0;
        x0 = x4 + x2; x4 -= x2;

        dst[5] = DCT_DESCALE(x1*postscale[5*8], postshift);
        dst[1] = DCT_DESCALE(x0*postscale[1*8], postshift);
        dst[7] = DCT_DESCALE(x4*postscale[7*8], postshift);
        dst[3] = DCT_DESCALE(x3*postscale[3*8], postshift);
    }
}
#endif

void MJpegWriterImpl::writeFrameData( const uchar* data, int step,
                                      int width, int height, int input_channels )
{
    //double total_cvt = 0, total_dct = 0;
    static bool init_cat_table = false;
    const int CAT_TAB_SIZE = 4096;
    static uchar cat_table[CAT_TAB_SIZE*2+1];
    if( !init_cat_table )
    {
        for( int i = -CAT_TAB_SIZE; i <= CAT_TAB_SIZE; i++ )
        {
            float a = (float)i;
            cat_table[i+CAT_TAB_SIZE] = (((int&)a >> 23) & 255) - (126 & (i ? -1 : 0));
        }
        init_cat_table = true;
    }

    //double total_dct = 0, total_cvt = 0;
    CV_Assert( data && width > 0 && height > 0 );

    // encode the header and tables
    // for each mcu:
    //   convert rgb to yuv with downsampling (if color).
    //   for every block:
    //     calc dct and quantize
    //     encode block.
    int x, y;
    int i, j;
    const int max_quality = 12;
    short fdct_qtab[2][64];
    unsigned huff_dc_tab[2][16];
    unsigned huff_ac_tab[2][256];

    int  x_scale = channels > 1 ? 2 : 1, y_scale = x_scale;
    int  dc_pred[] = { 0, 0, 0 };
    int  x_step = x_scale * 8;
    int  y_step = y_scale * 8;
    short  block[6][64];
    short  buffer[4096];
    int*   hbuffer = (int*)buffer;
    int  luma_count = x_scale*y_scale;
    int  block_count = luma_count + channels - 1;
    int  Y_step = x_scale*8;
    const int UV_step = 16;
    int u_plane_ofs = step*height;
    int v_plane_ofs = u_plane_ofs + step*height;

    if( quality < 1 ) quality = 1;
    if( quality > max_quality ) quality = max_quality;

    double inv_quality = 1./quality;

    // Encode header
    strm.putBytes( (const uchar*)jpegHeader, sizeof(jpegHeader) - 1 );

    // Encode quantization tables
    for( i = 0; i < (channels > 1 ? 2 : 1); i++ )
    {
        const uchar* qtable = i == 0 ? jpegTableK1_T : jpegTableK2_T;
        int chroma_scale = i > 0 ? luma_count : 1;

        strm.jputShort( 0xffdb );   // DQT marker
        strm.jputShort( 2 + 65*1 ); // put single qtable
        strm.putByte( 0*16 + i );   // 8-bit table

        // put coefficients
        for( j = 0; j < 64; j++ )
        {
            int idx = zigzag[j];
            int qval = cvRound(qtable[idx]*inv_quality);
            if( qval < 1 )
                qval = 1;
            if( qval > 255 )
                qval = 255;
            fdct_qtab[i][(idx/8) + (idx%8)*8] = (cvRound((1 << (postshift + 11)))/
                                        (qval*chroma_scale*idct_prescale[idx]));
            strm.putByte( qval );
        }
    }

    // Encode huffman tables
    for( i = 0; i < (channels > 1 ? 4 : 2); i++ )
    {
        const uchar* htable = i == 0 ? jpegTableK3 : i == 1 ? jpegTableK5 :
        i == 2 ? jpegTableK4 : jpegTableK6;
        int is_ac_tab = i & 1;
        int idx = i >= 2;
        int tableSize = 16 + (is_ac_tab ? 162 : 12);

        strm.jputShort( 0xFFC4 );      // DHT marker
        strm.jputShort( 3 + tableSize ); // define one huffman table
        strm.putByte( is_ac_tab*16 + idx ); // put DC/AC flag and table index
        strm.putBytes( htable, tableSize ); // put table

        BitStream::createEncodeHuffmanTable( BitStream::createSourceHuffmanTable(
                            htable, hbuffer, 16, 9 ), is_ac_tab ? huff_ac_tab[idx] :
                            huff_dc_tab[idx], is_ac_tab ? 256 : 16 );
    }

    // put frame header
    strm.jputShort( 0xFFC0 );          // SOF0 marker
    strm.jputShort( 8 + 3*channels );  // length of frame header
    strm.putByte( 8 );               // sample precision
    strm.jputShort( height );
    strm.jputShort( width );
    strm.putByte( channels );        // number of components

    for( i = 0; i < channels; i++ )
    {
        strm.putByte( i + 1 );  // (i+1)-th component id (Y,U or V)
        if( i == 0 )
            strm.putByte(x_scale*16 + y_scale); // chroma scale factors
        else
            strm.putByte(1*16 + 1);
        strm.putByte( i > 0 ); // quantization table idx
    }

    // put scan header
    strm.jputShort( 0xFFDA );          // SOS marker
    strm.jputShort( 6 + 2*channels );  // length of scan header
    strm.putByte( channels );          // number of components in the scan

    for( i = 0; i < channels; i++ )
    {
        strm.putByte( i+1 );             // component id
        strm.putByte( (i>0)*16 + (i>0) );// selection of DC & AC tables
    }

    strm.jputShort(0*256 + 63); // start and end of spectral selection - for
                                // sequental DCT start is 0 and end is 63

    strm.putByte( 0 );  // successive approximation bit position
                        // high & low - (0,0) for sequental DCT
    unsigned currval = 0, code = 0, tempval = 0;
    int bit_idx = 32;

    #define JPUT_BITS(val, bits) \
        bit_idx -= (bits); \
        tempval = (val) & bit_mask[(bits)]; \
        if( bit_idx <= 0 ) \
        {  \
            strm.jput(currval | ((unsigned)tempval >> -bit_idx)); \
            bit_idx += 32; \
            currval = bit_idx < 32 ? (tempval << bit_idx) : 0; \
        } \
        else \
            currval |= (tempval << bit_idx)

    #define JPUT_HUFF(val, table) \
        code = table[(val) + 2]; \
        JPUT_BITS(code >> 8, (int)(code & 255))

    // encode data
    for( y = 0; y < height; y += y_step, data += y_step*step )
    {
        for( x = 0; x < width; x += x_step )
        {
            int x_limit = x_step;
            int y_limit = y_step;
            const uchar* pix_data = data + x*input_channels;
            short* Y_data = block[0];

            if( x + x_limit > width ) x_limit = width - x;
            if( y + y_limit > height ) y_limit = height - y;

            memset( block, 0, block_count*64*sizeof(block[0][0]));

            if( channels > 1 )
            {
                short* UV_data = block[luma_count];
                // double t = (double)cv::getTickCount();

                if( colorspace == COLORSPACE_YUV444P && y_limit == 16 && x_limit == 16 )
                {
                    for( i = 0; i < y_limit; i += 2, pix_data += step*2, Y_data += Y_step*2, UV_data += UV_step )
                    {
#ifdef WITH_NEON
                        {
                            uint16x8_t masklo = vdupq_n_u16(255);
                            uint16x8_t lane = vld1q_u16((unsigned short*)(pix_data+v_plane_ofs));
                            uint16x8_t t1 = vaddq_u16(vshrq_n_u16(lane, 8), vandq_u16(lane, masklo));
                            lane = vld1q_u16((unsigned short*)(pix_data + v_plane_ofs + step));
                            uint16x8_t t2 = vaddq_u16(vshrq_n_u16(lane, 8), vandq_u16(lane, masklo));
                            t1 = vaddq_u16(t1, t2);
                            vst1q_s16(UV_data, vsubq_s16(vreinterpretq_s16_u16(t1), vdupq_n_s16(128*4)));

                            lane = vld1q_u16((unsigned short*)(pix_data+u_plane_ofs));
                            t1 = vaddq_u16(vshrq_n_u16(lane, 8), vandq_u16(lane, masklo));
                            lane = vld1q_u16((unsigned short*)(pix_data + u_plane_ofs + step));
                            t2 = vaddq_u16(vshrq_n_u16(lane, 8), vandq_u16(lane, masklo));
                            t1 = vaddq_u16(t1, t2);
                            vst1q_s16(UV_data + 8, vsubq_s16(vreinterpretq_s16_u16(t1), vdupq_n_s16(128*4)));
                        }

                        {
                            int16x8_t lane = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pix_data)));
                            int16x8_t delta = vdupq_n_s16(128);
                            lane = vsubq_s16(lane, delta);
                            vst1q_s16(Y_data, lane);

                            lane = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pix_data+8)));
                            lane = vsubq_s16(lane, delta);
                            vst1q_s16(Y_data + 8, lane);

                            lane = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pix_data+step)));
                            lane = vsubq_s16(lane, delta);
                            vst1q_s16(Y_data+Y_step, lane);

                            lane = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pix_data + step + 8)));
                            lane = vsubq_s16(lane, delta);
                            vst1q_s16(Y_data+Y_step + 8, lane);
                        }
#else
                        for( j = 0; j < x_limit; j += 2, pix_data += 2 )
                        {
                            int Y0, Y1, Y2, Y3, U, V;

                            Y_data[j] = pix_data[0] - 128;
                            Y_data[j+1] = pix_data[1] - 128;
                            Y_data[j+Y_step] = pix_data[step] - 128;
                            Y_data[j+Y_step+1] = pix_data[step+1] - 128;


                            UV_data[j>>1] = pix_data[v_plane_ofs] + pix_data[v_plane_ofs+1] +
                                    pix_data[v_plane_ofs+step] + pix_data[v_plane_ofs+step+1] - 128*4;
                            UV_data[(j>>1)+8] = pix_data[u_plane_ofs] + pix_data[u_plane_ofs+1] +
                                    pix_data[u_plane_ofs+step] + pix_data[u_plane_ofs+step+1] - 128*4;

                        }

                        pix_data -= x_limit*input_channels;
#endif
                    }
                }
                else
                {
                    for( i = 0; i < y_limit; i++, pix_data += step, Y_data += Y_step )
                    {
                        for( j = 0; j < x_limit; j++, pix_data += input_channels )
                        {
                            int Y, U, V;

                            if( colorspace == COLORSPACE_BGR )
                            {
                                int r = pix_data[2];
                                int g = pix_data[1];
                                int b = pix_data[0];

                                Y = DCT_DESCALE( r*y_r + g*y_g + b*y_b, fixc) - 128;
                                U = DCT_DESCALE( r*cb_r + g*cb_g + b*cb_b, fixc );
                                V = DCT_DESCALE( r*cr_r + g*cr_g + b*cr_b, fixc );
                            }
                            else if( colorspace == COLORSPACE_RGBA )
                            {
                                int r = pix_data[0];
                                int g = pix_data[1];
                                int b = pix_data[2];

                                Y = DCT_DESCALE( r*y_r + g*y_g + b*y_b, fixc) - 128;
                                U = DCT_DESCALE( r*cb_r + g*cb_g + b*cb_b, fixc );
                                V = DCT_DESCALE( r*cr_r + g*cr_g + b*cr_b, fixc );
                            }
                            else
                            {
                                Y = pix_data[0] - 128;
                                U = pix_data[v_plane_ofs] - 128;
                                V = pix_data[u_plane_ofs] - 128;
                            }

                            int j2 = j >> (x_scale - 1);
                            Y_data[j] = (short)Y;
                            UV_data[j2] = (short)(UV_data[j2] + U);
                            UV_data[j2 + 8] = (short)(UV_data[j2 + 8] + V);
                        }

                        pix_data -= x_limit*input_channels;
                        if( ((i+1) & (y_scale - 1)) == 0 )
                        {
                            UV_data += UV_step;
                        }
                    }
                }

               // total_cvt += (double)cv::getTickCount() - t;
            }
            else
            {
                for( i = 0; i < y_limit; i++, pix_data += step, Y_data += Y_step )
                {
                    for( j = 0; j < x_limit; j++ )
                        Y_data[j] = (short)(pix_data[j]*4 - 128*4);
                }
            }

            for( i = 0; i < block_count; i++ )
            {
                int is_chroma = i >= luma_count;
                int src_step = x_scale * 8;
                int run = 0, val;
                const short* src_ptr = block[i & -2] + (i & 1)*8;
                const unsigned* htable = huff_ac_tab[is_chroma];

                //double t = (double)cv::getTickCount();
                aan_fdct8x8( src_ptr, buffer, src_step, fdct_qtab[is_chroma] );
                //total_dct += (double)cv::getTickCount() - t;

                j = is_chroma + (i > luma_count);
                val = buffer[0] - dc_pred[j];
                dc_pred[j] = buffer[0];
                
                {
                    int cat = cat_table[val + CAT_TAB_SIZE];
                    
                    //CV_Assert( cat <= 11 );
                    JPUT_HUFF( cat, huff_dc_tab[is_chroma] );
                    JPUT_BITS( val - (val < 0 ? 1 : 0), cat );
                }
                
                for( j = 1; j < 64; j++ )
                {
                    val = buffer[zigzag[j]];
                    
                    if( val == 0 )
                    {
                        run++;
                    }
                    else
                    {
                        while( run >= 16 )
                        {
                            JPUT_HUFF( 0xF0, htable ); // encode 16 zeros
                            run -= 16;
                        }
                        
                        {
                            int cat = cat_table[val + CAT_TAB_SIZE];
                            //CV_Assert( cat <= 10 );
                            JPUT_HUFF( cat + run*16, htable );
                            JPUT_BITS( val - (val < 0 ? 1 : 0), cat );
                        }
                        
                        run = 0;
                    }
                }
                
                if( run )
                {
                    JPUT_HUFF( 0x00, htable ); // encode EOB
                }
            }
        }
    }
    
    // Flush
    JPUT_BITS((unsigned)-1, bit_idx & 31);
    strm.jputShort( 0xFFD9 ); // EOI marker
    /*printf("total dct = %.1fms, total cvt = %.1fms\n",
           total_dct*1000./cv::getTickFrequency(),
           total_cvt*1000./cv::getTickFrequency());*/
}

Ptr<MJpegWriter> openMJpegWriter(const std::string& filename, Size size, double fps, int colorspace)
{
    Ptr<MJpegWriter> mjcodec = new MJpegWriterImpl(filename, size, fps, colorspace);
    if( mjcodec->isOpened() )
        return mjcodec;
    return Ptr<MJpegWriter>();
}

}
}

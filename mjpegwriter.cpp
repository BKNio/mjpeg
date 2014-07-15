/* ��������� ����������
MainAVIHeader:
DWORD dwMicroSecPerFrame;    //  ���������� ����������� ����� ������� ��� ����� �����
DWORD dwMaxBytesPerSec;      //  ��������� ������������ �������� �������� ������ �����
DWORD dwReserved1;           // NULL
DWORD dwFlags;               // ??
DWORD dwTotalFrames;         // ����� ����� ������, ��������� � ����� �����
DWORD dwInitialFrames;       // ������������
DWORD dwStreams;             // ����� ������� 2: ����� + �����
DWORD dwSuggestedBufferSize; // ������ ������ ��� �������� chunka'�. ���� 0, �� ����� ������ ��������� ��� ���������������
DWORD dwWidth;               // ������ �����
DWORD dwHeight;              // ������ �����
DWORD dwReserved[4];         // NULL, NULL, NULL, NULL
AVIStreamHeader:
FOURCC fccType;              // ����� ����� fourcc 'vids','auds', 'txts'...
FOURCC fccHandler;           // cvid 
DWORD dwFlags;               // NULL
DWORD dwPriority;            // NULL
DWORD dwInitialFrames;       // NULL
DWORD dwScale;               // NULL
DWORD dwRate;                // NULL
DWORD dwStart;               // NULL
DWORD dwLength;              // NULL
DWORD dwSuggestedBufferSize; // NULL
DWORD dwQuality;             // NULL
DWORD dwSampleSize;          // NULL
RECT rcFrame;
*/
#include "mjpegwriter.hpp"
#include <fstream>

typedef unsigned long DWORD;
typedef unsigned char BYTE;
typedef DWORD FOURCC;
typedef unsigned short WORD;
#define fourCC(a,b,c,d) ( (DWORD) (((d)<<24) | ((c)<<16) | ((b)<<8) | (a)) )
#define NUM_MICROSEC_PER_SEC 1000000
#define MAX_BYTES_PER_SEC 314572800         // 300 Mb/s
#define STREAMS 1

MjpegWriter::MjpegWriter() : isOpen(false), outFile(NULL), outformat(AVI), outfps(20) { }

int MjpegWriter::Open(const char* outfile, uchar format, uchar fps)
{
    if (fps < 1) return -3;
    switch (format)
    {
        case AVI:
        {
            if (!(outFile = fopen(outfile, "wb+")))
                return -1;
            // Record MainAviHeader. �� ������ ����� �������� ������
            DWORD StartAVIRIFF[22] = { fourCC('R', 'I', 'F', 'F'), NULL, fourCC('A', 'V', 'I', ' '),
                fourCC('L', 'I', 'S', 'T'), NULL, fourCC('h', 'd', 'r', 'l'), fourCC('a', 'v', 'i', 'h'),
                sizeof(DWORD) * 14, 1.0f * NUM_MICROSEC_PER_SEC / fps, MAX_BYTES_PER_SEC, NULL,
                NULL, NULL, NULL, STREAMS, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
            fwrite(StartAVIRIFF, sizeof(DWORD), 22, outFile);
            // Record StreamHeader. �� ������ ����� �������� ������
            DWORD StreamHEAD[18] = { fourCC('L', 'I', 'S', 'T'), NULL, fourCC('s', 't', 'r', 'l'), fourCC('s', 't', 'r', 'h'),
                sizeof(DWORD)* 13 + sizeof(WORD), fourCC('v', 'i', 'd', 's'), fourCC('c', 'v', 'i', 'd'), NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL};
            outfps = fps;
            break;
        }
        default: 
            return -2;
    }
    
    isOpen = true;
    return 1;
}

int MjpegWriter::Close()
{
    // ����� ������ �������� � ���������� ��������� AVI
    if (fclose(outFile))
        return -1;
    isOpen = false;
    // ����� ��������, ���� � ���� �� ��� ������� �� ���� ����, ������� ����
    return 1;
}

int MjpegWriter::Write(const Mat Im)
{
    if (!isOpen) return -1;
    // ��������� ������, ��� ���������� jpeg �������
    return 1;
}

bool MjpegWriter::isOpened()
{
    return isOpen;
}

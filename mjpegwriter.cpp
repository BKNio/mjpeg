#include "mjpegwriter.hpp"
#include <fstream>

MjpegWriter::MjpegWriter() : isOpen(false), outFile(NULL), outformat(1), outfps(20) { }

int MjpegWriter::Open(const char* outfile, uchar format, uchar fps)
{
    switch (format)
    {
        case 1:                 // if AVI
            if (outFile = fopen(outfile, "wb+"))
                return -1;
            // ����� ������ ��������� �����
            outfps = fps;
            break;
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
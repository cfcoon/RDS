/*
MIT License

Copyright(c) 2026 by anon, released into public domain

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

None.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#undef UNICODE
#include <windows.h>
#pragma comment(lib,"ws2_32")

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <conio.h>  

//Options
#define SEND_BUF_SIZE 256        //Outgoing packet buffer to GD2015 serial port, actually we only need 128 bytes  
#define RECV_BUF_SIZE 256        //Receive buffer from UDP listening port, should be enough for artist - title string  
#define SEND_DELAY_MS 200        //Delay in milliseconds between sending each packet to GD2015, it chokes if < 100
#define LISTEN_INTERVAL_MS 3000  //Interval in milliseconds to check for new incoming data
#define DEFAULT_LISTEN_PORT 21100//Default listening port for plain ASCII RT text or PS= and RT= style commands
#define SCROLL_DELAY_MS 4000     //Length of time each RT word will appear in PS field (plus send time) if in PS scroll mode
#define FIX_ALL_CAPS    true     //Whether to fix all caps in RT messages (Sometimes artist-song text is all caps)
#define SPLIT_LONG_RT   true     //Whether to split up RT messages > 32 length and timeshare the two every RT_SWAP_DELAY_MS
#define RT_SWAP_DELAY_MS 10000   //Timeshare duration for split RT message if SPLIT_LONG_RT true   
#define DEFAULT_PTY 22            
#define DEFAULT_PI 0x1234
#define DEFAULT_PS "RADIO-FM"
#define DEFAULT_SERIAL_PORT "COM3"

//Globals
HANDLE hSerialPort = 0;          //Serial port handle
SOCKET UDPSocket = NULL;         //listening socket
uint8_t PacketBuffer[SEND_BUF_SIZE];   //Buffer we assemble RDS packets to  
unsigned int NextPacketBufferSpot = 0; //offset to next open spot in outgoing packet buffer we're appending to
char RecvBuffer[RECV_BUF_SIZE]; //data received from UDP listening port or copied in from text file we're watching
char FilePathAndName[MAX_PATH]; //text file to monitor if non-zero 

bool bPSScrollMode = false;      //True if in PS scroll mode, see SetPS
size_t ScrollWordIndex = 0;      //Index into RT string to word currently displaying in PS (if in scroll mode) 
ULONGLONG LastScrollTime = 0;    //GetTickCount64() time the last scrolling PS word was set

bool bRTSplit = false;           //True if alternating half of RT string then other half
ULONG RTSplitShowing = 0;        //1 if RTSplit1 is currently showing, 2 if 2
ULONGLONG LastRTSwitchTime = 0;
char RTSplit1[33];               //Zero-terminated, non-padded split RT text if bRTSplit
char RTSplit2[33];

//Latest RDS data the GD-2015 expects us to keep refreshing
uint8_t PTY = DEFAULT_PTY;                 
uint16_t PI = DEFAULT_PI;
char PS[8] = { 0 }; 
char RT[64]= { 0 };

//Forward declarations
static int ResetScroll();

//Initialize by opening Windows serial port (e.g. "COM3") and UDP port to listen on,
// returns 1 if success, 0 if fail
static int Initialize(const char* SerialPort, unsigned int UDPListenPort)
{
    FilePathAndName[0] = 0;

    //Open the outgoing serial port to the transmitter
    DCB dcbSerialParams = { 0 };
    COMMTIMEOUTS timeouts = { 0 };

    hSerialPort = CreateFile(SerialPort, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSerialPort == INVALID_HANDLE_VALUE)
    {
        printf("Error opening serial port, is another app using it?\n");
        return 0;
    }

    //Get current settings 
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerialPort, &dcbSerialParams))
    {
        printf("Error getting COM port state\n");
        CloseHandle(hSerialPort);
        return 0;
    }

    //Set comm parameters  
    dcbSerialParams.BaudRate = 9600;   //faster doesn't seeem to work with GD2015
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hSerialPort, &dcbSerialParams))
    {
        printf("Error setting device state\n");
        CloseHandle(hSerialPort);
        return 0;
    }

    //Set timeouts though these are probably the defaults anyway
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerialPort, &timeouts);

    //Initialize network connection, Windows specific
    WORD versionRequested = MAKEWORD(2, 0);
    WSADATA wsaData;
    if (WSAStartup(versionRequested, &wsaData) != 0)
    {
        printf("Network failed, check app permissions and LAN connection\n");
        return 0;
    }

    //Create UDP listening socket
    UDPSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (UDPSocket == SOCKET_ERROR)
    {
        printf("Network problem, can't create listening socket\n");
        return 0;
    }

    //Set to non-blocking -- we poll it and don't want to wait each time
    u_long DontBlock = true;
    if (ioctlsocket(UDPSocket, FIONBIO, &DontBlock) != 0)
    {
        closesocket(UDPSocket);
        UDPSocket = NULL;
        printf("Network problem, failed setting receive socket to non-blocking\n");
        return 0;
    }

    //bind it to the port
    int rc;
    struct sockaddr_in serveraddr;
    int serveraddrlen = sizeof(serveraddr);
    memset(&serveraddr, 0x00, serveraddrlen);
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons((unsigned short)UDPListenPort);
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    rc = bind(UDPSocket, (struct sockaddr*)&serveraddr, serveraddrlen);
    if (rc < 0)
    {
        printf("Network problem, couldn't bind to UDP receive port\n");
        closesocket(UDPSocket);
        UDPSocket = NULL;
        return 0;
    }

    printf("Listening on port %i\n", UDPListenPort);

    return 1;
}

//Add given byte to outgoing packet buffer
static int AddByteToBuffer(uint8_t Byte)
{
    if (NextPacketBufferSpot >= SEND_BUF_SIZE)
        return 0;
    PacketBuffer[NextPacketBufferSpot++] = Byte;
    return 1;
}

//Send the packet buffer given the number of 8 byte packets in it, one packet at a time,
//ending the packet with 0x0a and waiting between packets SendDelay milliseconds
//(so this may take awhile to return). 0x0a thing is apparently specific to GD2015, 
//not a standard
static int SendBuffer(int NumPackets)
{
    uint8_t CR = 0x0a;
    DWORD Written;
    for (int i = 0; i < NumPackets; i++)
    {
        WriteFile(hSerialPort, &PacketBuffer[i * 8], 8, &Written, nullptr);
        WriteFile(hSerialPort, &CR, 1, &Written, nullptr);
        Sleep(SEND_DELAY_MS);
    }

    return 1;
}

//Set the PS field and stream it out, max 8 characters not including terminating 0, 
//any more won't be sent. Automatically pads with spaces if less than 8, per the protocol.
//Note %%%% means enable scrolling mode where one word at a time of
//RT string will appear in the PS field (words bigger than 8 chars will be clipped).
//That option should only be done in US where it is already common, elsewhere radio
//manufacturers may be assuming it's static and it could mess up presets or other
//bad things (RDS spec says it's meant for static station ID). (^^^^ means disable
//scroll mode). Check other local stations if they're using scroll. Some older 
//radios don't show RT, only PS field so that's why they do it. 
static int SetPS(const char* PSString, bool bNew)
{
    if (bNew)
    {
        if (PSString)
        {
            //Check for enable scroll mode
            if (PSString[0] == '%' &&
                PSString[1] == '%' &&
                PSString[2] == '%' &&
                PSString[3] == '%')
            {
                bPSScrollMode = true;
                printf("PS scroll mode enabled\n");
                ResetScroll();  //this will call us back recursively
                return 1;
            }
            //Check for disable
            if (PSString[0] == '^' &&
                PSString[1] == '^' &&
                PSString[2] == '^' &&
                PSString[3] == '^')
            {
                bPSScrollMode = false;
                printf("PS scroll mode disabled\n");
                return 1;
            }
            //Copy into cached PS string (not zero terminated) and fill excess with spaces
            size_t len = PSString ? strlen(PSString) : 0;
            for (int i = 0; i < 8; i++)
            {
                if (i < len)
                    PS[i] = PSString[i];
                else
                    PS[i] = ' ';
            }
        }
        else
            strcpy_s(PS, 8, DEFAULT_PS);

        if (!bPSScrollMode)
        {
            //Don't show every single word change in scroll mode, just static
            if (PSString)
                printf("PS set: %s\n", PSString);
            else
                printf("PS set: %s\n", DEFAULT_PS);
        }
    }

    //reset the buffer
    NextPacketBufferSpot = 0;

    //Assemble the four PS packets into the buffer
    for (int i = 0; i < 4; i++)
    {
        //2 bytes: high and low PI bytes
        AddByteToBuffer(PI >> 8);    //high byte
        AddByteToBuffer(PI & 0x00FF);//low

        //2 bytes that are bitfields, first 4 bits are packet type = 0 0 0 0 (PS), 
        // two bits 0 0 (using version A), 5 bits PTY, two bits 0 1, then 3 bits sequence 
        // number
        uint16_t Bits16 = (PTY << 5) | (1 << 3) | i;
        AddByteToBuffer(Bits16 >> 8);
        AddByteToBuffer(Bits16 & 0xFF);

        //4 bytes payload but first two are always 0 (or otherwise ignored), then 2 
        //characters of the PS string at a time. 
        AddByteToBuffer(0);
        AddByteToBuffer(0);
        AddByteToBuffer(PS[i * 2]);
        AddByteToBuffer(PS[i * 2 + 1]);
    }
    SendBuffer(4);

    return 1;
}

//Set the PI field, should only be done once
static int SetPI(char* HexString)
{
    char* endptr;
    unsigned long d = strtol(HexString, &endptr, 16);
    if (endptr == HexString || d > 0xFFFF)
    {
        printf("Invalid PI, ignoring: ");
        printf(HexString);
        printf("\n");
        return 0;
    }
    PI = d & 0xFFFF;
    printf("PI set: %X\n", PI);
    return 1;
}

//Set the PTY field
static int SetPTY(uint8_t PTYNum)
{
    if (PTYNum > 31)
    {
        printf("Invalid PTY, ignoring: %i\n", PTYNum);
        return 0;
    }
    PTY = PTYNum;
    printf("PTY set: %i\n", PTY);
    return 1;
}

//Load cached RT string with RTSplit1 or 2
static int SwapSplitRT()
{
    char* str = NULL;
    if (RTSplitShowing == 1)
    {
        str = RTSplit2;
        RTSplitShowing = 2;
    }
    else
    {
        str = RTSplit1;
        RTSplitShowing = 1;
    }
    strcpy_s(RT, 64, str);

    //Pad rest of RT with spaces
    size_t len = strlen(RT);
    for (size_t i = len; i < 64; i++)
    {
        RT[i] = ' ';
    }

    LastRTSwitchTime = GetTickCount64();

    return 1;
}

//Set the given RT zero-terminated string and stream it out. Set bNew to true if 
//this is new data (to request screen clear on receivers) versus just a refresh 
//message. Usually it has "artist - title" with space-dash-space delimeter which
//we use to intellegently truncate or split if the whole string is longer than
//the 64 character RT string limit. 
static int SetRT(char* RTString, bool bNew)
{
    if (bNew)
    {
        //Copy into cached RT string (not zero terminated) and fill excess with spaces
        size_t len = RTString? strlen(RTString) : 0;
        bool bCapsFixed = false;
        for (int i = 0; i < 64; i++)
        {
            if (i < len)
            {
                RT[i] = RTString[i];

                //Fix all-caps text: if character before is another letter, ensure small letter
                if (FIX_ALL_CAPS)
                {
                    if (i > 0 && (((RT[i - 1] >= 'A' && RT[i - 1] <= 'Z') ||
                        (RT[i - 1] >= 'a' && RT[i - 1] <= 'z')) &&
                        (RTString[i] >= 'A' && RTString[i] <= 'Z')))
                    {
                        RT[i] += 'a' - 'A';
                        RTString[i] += 'a' - 'A';
                        bCapsFixed = true;
                    }
                }
            }
            else
                RT[i] = ' ';
        }

        //If it's a super long artist - title string (more than 64 chars), copy the title part down 
        //so probably excessively long artist part is clipped, but we keep the full (probably
        //shorter) title that will get split into alternating transmissions. If both are
        //too long at least we'll show more song title than artist
        if (len > 64)
        {
            char* minus = strstr(RTString, " - "); //start of space-minus-space in the string
            if (minus)
            {
                size_t TitleLength = strlen(minus);
                if (TitleLength < 64)
                {
                    memcpy_s(&RTString[64 - TitleLength], TitleLength, minus, TitleLength);
                    RTString[64] = 0; 

                    //Call this again (yay recursion) because we may need to fix caps in the new 
                    //shifted string. Len is < 64 now from previous line so it won't get back 
                    //to here and infinite loop. 
                    return SetRT(RTString, true); 
                }
            }
            len = 64;
        }

        //Determine if we should split it 
        if (SPLIT_LONG_RT && len > 32)
        {
            //See if ' - ' or only one - present in the string (typical artist/title delimineter)
            int NumMinuses = 0;
            int MinusIndex = 0;
            int SplitPointLow = 0;
            int SplitPointHi = 0;
            bool bSpaceMinusSpaceFound = false;
            int j = 0;
            for (j = 0; j < len && NumMinuses <= 1 && !bSpaceMinusSpaceFound; j++)
            {
                if (RT[j] == '-')
                {
                    NumMinuses++;
                    MinusIndex = j;
                    if (j > 0 && RT[j - 1] == ' ' && j < len - 1 && RT[j + 1] == ' ')
                        bSpaceMinusSpaceFound = true;
                }
            }
            //Found likely artist - title split point, check if splitting there will fit cleanly
            if (bSpaceMinusSpaceFound || NumMinuses == 1)
            {
                SplitPointLow = MinusIndex - 1;
                SplitPointHi = MinusIndex + 1;
                if (bSpaceMinusSpaceFound)
                {
                    SplitPointLow--; //Don't include space before and after
                    SplitPointHi++;
                }
                //One won't fit, set to even split
                if (SplitPointLow > 31 || (len - SplitPointHi) > 31)
                {
                    SplitPointLow = 31;
                    SplitPointHi = 32;
                }
            }
            else
            {
                SplitPointLow = 31;
                SplitPointHi = 32;
            }

            //Copy in text up to split points (zero terminated)
            for (j = 0; j <= SplitPointLow; j++)
            {
                RTSplit1[j] = RT[j];
            }
            RTSplit1[j] = 0;
            int k = 0;
            for (j = SplitPointHi, k = 0; j < len; j++, k++)
            {
                RTSplit2[k] = RT[j];
            }
            RTSplit2[k] = 0;

            bRTSplit = true;
            RTSplitShowing = 2;  //swap will load up RTSplit1 string into RT 
            SwapSplitRT();
        }
        else
            bRTSplit = false;

        if (RTString)
        {
            printf("RT set: %s\n", RTString);
            if (bRTSplit)
                printf("  Split into:\n  %s\n  %s\n", RTSplit1, RTSplit2);
            if (bCapsFixed)
                printf("  (All-caps fixed)\n");
        }
        else
            printf("RT cleared\n");

        //Reset PS scroll if we were scrolling
        if (bPSScrollMode)
            ResetScroll();
    }

    //reset the buffer
    NextPacketBufferSpot = 0;

    //Assemble the 16 packets into the buffer
    for (int i = 0; i < 16; i++)
    {
        //2 bytes: high and low PI bytes
        AddByteToBuffer(PI >> 8);
        AddByteToBuffer(PI & 0x00FF);

        //2 bytes that are bitfields, first 4 bits packet type = 0 0 1 0 = RT, then two  
        // zero bits (to indicate we're using version A format of the RT message, and 
        // we don't provide traffic information), 5 bits PTY, 1 bit screen clear request 
        // then 4 bits sequence number
        uint16_t Bits16 = (1 << 13) | (PTY << 5) | (bNew? 1 << 4 : 0) | i;
        AddByteToBuffer(Bits16 >> 8);
        AddByteToBuffer(Bits16 & 0xFF);

        //4 bytes payload that are 4 characters of the RT string at a time 
        AddByteToBuffer(RT[i * 4]);
        AddByteToBuffer(RT[i * 4 + 1]);
        AddByteToBuffer(RT[i * 4 + 2]);
        AddByteToBuffer(RT[i * 4 + 3]);
    }
    SendBuffer(16);

    return 1;
}

//See if we have any new data sent to us on the UDP port or in the file we're
//supposed to monitor, and if so update the RDS fields. Return 1 if new data 
//received, 0 if nothing received
static int CheckNewInput()
{
    //Monitoring a file?
    size_t FileRead = 0;
    if (FilePathAndName[0])
    {
        FILE* fp = NULL;
        fopen_s(&fp, FilePathAndName, "r");
        if (fp)
        {
            FileRead = fread(RecvBuffer, 1, RECV_BUF_SIZE, fp);
            fclose(fp);
        }
    }

    //Check listening port
    int BytesReceived = recv(UDPSocket, RecvBuffer, RECV_BUF_SIZE, 0);
    if (FileRead == 0)
    {
        if (BytesReceived == 0)
            return 0;
        if (BytesReceived == SOCKET_ERROR)
        {
            int ErrNo = WSAGetLastError();
            if (ErrNo != WSAEWOULDBLOCK)    
                printf("Network error\n");
     
            //WSAEWOULDBLOCK isn't an error, just means no data received.
            return 0;
        }
    }
    else if (BytesReceived == 0)
        BytesReceived = (int)FileRead;

    //Binary UECP format? Used by some automation systems
    if (RecvBuffer[0] == 0xFE)
    {
        printf("UECP protocol not supported, try setting to text/ASCII\n");
        return 0;
    }
 
    //Not binary so assume it's text, ensure zero terminated 
    RecvBuffer[(BytesReceived < RECV_BUF_SIZE? BytesReceived : RECV_BUF_SIZE - 1)] = 0;

    //Check for "RT=" style protocol
    if (strstr(RecvBuffer, "="))
    {
        //RT?
        char* Field = strstr(RecvBuffer, "RT=");
        if (Field)
        {
            Field += 3;
            SetRT(Field, true);
            return 1;
        }

        //PS?
        Field = strstr(RecvBuffer, "PS=");
        if (Field)
        {
            Field += 3;
            SetPS(Field, true);
            return 1;
        }

        //PI?
        Field = strstr(RecvBuffer, "PI=");
        if (Field)
        {
            Field += 3;
            SetPI(Field);
            return 1; 
        }

        //PTY?
        Field = strstr(RecvBuffer, "PTY=");
        if (Field)
        {
            Field += 3;
            SetPTY(atoi(Field));
            return 1;
        }

        //Likely other RDS field we don't support? 
        if ((BytesReceived > 2 && RecvBuffer[2] == '=') || (BytesReceived > 3 && RecvBuffer[3] == '='))
        {
            printf("Unsupported RDS field, ignoring: %s\n", RecvBuffer);
            return 0;
        }
        
        //Otherwise fall through, may just have = sign in RT string somewhere 
    }

    //Plain ASCII i.e. RT string
    SetRT(RecvBuffer, true);
    return 1;
}

//Load up the current scroll index word into PS 
static int LoadScrollWord()
{
    //Copy over until 8 characters reached, Max RT string reached, or space is found
    char Buff[9] = { 0 };
    size_t RTPtr = ScrollWordIndex;
    size_t BuffPtr = 0;
    Buff[0] = 0;

    for (BuffPtr = 0; BuffPtr < 8 && RTPtr < 63 && RT[RTPtr] != ' '; BuffPtr++, RTPtr++)
    {
        //Don't include parentheses
        if (RT[RTPtr] != '(' && RT[RTPtr] != ')')
            Buff[BuffPtr] = RT[RTPtr];
        else
            RTPtr++;
    }
    Buff[BuffPtr] = 0;
    
    SetPS(Buff, true);
    LastScrollTime = GetTickCount64();
    return 1;
}

//Reset scroll index to first word of RT string 
static int ResetScroll()
{
    ScrollWordIndex = 0;
    return LoadScrollWord();
}

//Update scroll if time, assumes scroll mode active
static int UpdateScroll()
{
    //Not time to change yet?
    if (GetTickCount64() < (LastScrollTime + SCROLL_DELAY_MS))
        return 1;

    //Advance to space after current word
    while (RT[ScrollWordIndex] != ' ' && ScrollWordIndex < 63)
        ScrollWordIndex++;

    //Advance to next non-space (or end of buffer)
    //But skip over '-' separators
    while ((RT[ScrollWordIndex] == ' ' || RT[ScrollWordIndex] == '-') 
        && ScrollWordIndex < 63)
        ScrollWordIndex++;

    //End of buffer, go back to beginning
    if (ScrollWordIndex == 63)
        return ResetScroll();

    return LoadScrollWord();
}

//Update split RT timesharing
static int UpdateRTSplit()
{
    //Not time yet?
    if (GetTickCount64() < (LastRTSwitchTime + RT_SWAP_DELAY_MS))
        return 1;

    SwapSplitRT();
    return 1;
}
static int Shutdown()
{
    if (UDPSocket)
    {
        shutdown(UDPSocket, 0);
        closesocket(UDPSocket);
        UDPSocket = NULL;
        WSACleanup();
    }
    CloseHandle(hSerialPort);
    hSerialPort = NULL;
    return 1;
}

static int WaitForKeyHit()
{
    printf("\nPress any key to exit...");
    while (!_kbhit())
        Sleep(100);
    return 1;
}

int main(int argc, char* argv[])
{
    printf("RDS Driver for GD-2015 series transmitter\n\n");
    DWORD UDPPort = DEFAULT_LISTEN_PORT;

    //Set initial values from command line parameters 
    if (argc > 1)
    {
        if (argv[1][0] == '?')
        {
            printf("\nGD2015 RDS Driver\n\n");
            printf("Command line parameters:\n  [serial port e.g. COM3]\n  [PI 4 digit hex code, e.g. 0123]\n");
            printf("  [PTY code 0 - 31]\n  [Initial PS text]\n  [Initial RT text]\n");
            printf("  [optional:UDP listen port e.g. 21100 OR path and filename to text file to monitor e.g. c:\\dir\\NowPlaying.txt]\n\n");
            WaitForKeyHit();
            return 0;
        }

        if (argc > 2 && argv[2][0])
            SetPI(argv[2]);
        if (argc > 3 && argv[3][0])
            SetPTY(atoi(argv[3]));
        if (argc > 4 && argv[4][0])
            SetPS(argv[4], true);
        if (argc > 5 && argv[5][0])
            SetRT(argv[5], true);
        if (argc > 6 && argv[6][0])
        {
            //Port number?
            if (argv[6][0] >= '0' && argv[6][0] <= '9')
            {
                int d = atoi(argv[6]);
                if (d < 0 || d > 65535)
                {
                    printf("Invalid port number\n");
                    WaitForKeyHit();
                    return 1;
                }
                UDPPort = d;
            }
            //Assume it's path and filename to text file to monitor
            else
                strcpy_s(FilePathAndName, MAX_PATH, argv[6]);
        }
    }
    else
    {
        SetPS(DEFAULT_PS, true);
    }

    if ((argc > 1 && !Initialize(argv[1], UDPPort)) ||
        (argc <= 1 && !Initialize(DEFAULT_SERIAL_PORT, UDPPort)))
    {
        WaitForKeyHit();
        return 1;
    }

    ULONGLONG LastUpdateTime = GetTickCount64() - LISTEN_INTERVAL_MS - 1;
    ULONGLONG CurrentTime = 0;

    //Run forever, can close console window to exit 
    while (1 > 0)
    {
        CurrentTime = GetTickCount64();
        if (abs((long long)(CurrentTime - LastUpdateTime)) >= LISTEN_INTERVAL_MS)
        {
            LastUpdateTime = CurrentTime;
            CheckNewInput();
        }

        SetPS(PS, false);
        SetRT(RT, false);
        
        if (bPSScrollMode)
            UpdateScroll();
        if (bRTSplit)
            UpdateRTSplit();
        
        Sleep(1000);
    }
    
    Shutdown();

    return 0;
}
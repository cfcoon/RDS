//MIT License, released to Public Domain Nov. 2026
// 
//This is an example Windows console program for the GD-2015 and 
//GD-2025 FM transmitter that sets RDS data through its RS-232 
//interface. PI, PTY, PS and RT fields can be set with this code.
// 
//How to use:
// 
//First get a USB to RS-232 adapter and figure out which COM 
//port it's assigned to (no null modem adapter needed, just
//plug straight in). Put the assigned COM port in the "COM3" 
//part in main() below if different, along with your desired PI, 
//PTY, and PS and RT text.
// 
//Then power up your transmitter and launch this code. Press 
//any key to exit (it may take a few seconds). Note an FM receiver 
//may take ~30 seconds to update all RDS data in its display when 
//it gets a new RDS stream.  
// 
//Programming:
//
//Protocol to communicate to the transmitter: first open the 
//serial port with 9600,8,1,n.
//
//Then there are two possible messages the transmitter accepts, 
//one sets PI, PTY and PS fields (SetPS function) and the other 
//sets PI, PTY and RT fields (SetRT function). It expects you to 
//continuously set them (which is about every three seconds in 
//this example, based on the default delay between packets) 
//probably because it just passes it through to its encoder so 
//it's broadcast continuously.
// 
//The messages (PS and RT) have different sizes, and apparently
//have to be sent 8 bytes at a time (called "packets" below),
//followed by a carriage return 0x0A. So really 9 bytes at a time 
//and it needs some kind of delay between packets so it can 
//process them (100ms seems to work okay but default is 200). 
// 
//Note the GD- series transmitters can operate in two RDS modes, 
//either sending saved RDS data or sending out live RDS data 
//received through the port. That's set in the "RDS DAT" page. 
//If you select "UNSAVE" it will clear saved data and go off 
//live stream from RS-232. If you select "WRITE" then "SAVE" it 
//will save its current RDS data and keep transmitting that even 
//after power off and on, and ignore any RS-232 inputs when it's
//in that mode.  
//
//Also it sometimes needs a reset with power off and on if it's not 
//setting data like you expect (especially when you're not 
//continuously updating the stream).
//
//The 8 byte packet format appears to follow
//https://en.wikipedia.org/wiki/Radio_Data_System in the 
//"baseband coding" section. It consists of:
// 
//Header ("Block 1" and "Block 2" in the reference material):
// 
//2 bytes: high byte PI, low byte PI then
//2 bytes: first 4 bits are packet/group type (0000 for PS, 0010 for RT) 
//then 1 bit that this code sets to 0 to always use variant A (1 = B)
//then 1 bit also set to 0 (flag that the station provides traffic messages)
//then 5 bits PTY 
//then if packet type PS: 2 bits 01, then 3 bits packet sequence# xxx
//or if packet type RT: 1 bit 0 (1 is request screen clear), then 4 
//   bits packet sequence# xxxx
//
//Following that are 4 bytes of payload (Blocks 3 and 4):
// 
//For packet type PS: 0x00 0x00 char1 char2  (4 packets get sent in a 
//row for 8 text chars total, padded with trailing spaces if less)
//
//For packet type RT: char1 char2 char3 char4 (16 packets sent in a 
//row for 64 text chars total, padded with trailing spaces).
//
//What API the GD series transmitters use was not published anywhere 
//so most of this was reverse engineered from the RDS app the 
//manufacturer provides:
//
//https://github.com/Chinabroadcast/GD-2015-RDS-software 
//("Installer-Mario" file)

#undef UNICODE
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <conio.h>  

//Globals
#define BUF_SIZE 256 
#define SEND_DELAY_MS 200        //Delay in milliseconds between sending each packet so transmitter doesn't choke
HANDLE hPort = 0;                //Serial port handle
uint8_t Buffer[BUF_SIZE];        //Send buffer we assemble packets to, most we need is actually 128  
unsigned int NextBufferSpot = 0; //offset to next open spot in buffer we're appending to
uint8_t PTY = 0;                 //Saved PTY and PI values that have to be put in each PS and RT message 
uint16_t PI = 0x0000;  

//Initialize by opening Windows serial port (e.g. "COM3") and cache PI hex code (e.g. 0x1234) and 
//PTY code (e.g. 1 = "News station"). PI and PTY won't be sent until PS or RT text is sent, just 
//how the protocol works. Returns 1 if success, 0 if fail
static int Initialize(const char* COMPort, uint16_t PI_HexCode, uint8_t PTY_Number)
{
    PTY = PTY_Number;
    PI = PI_HexCode;

    //Open the serial port
    DCB dcbSerialParams = { 0 };
    COMMTIMEOUTS timeouts = { 0 };

    hPort = CreateFile(COMPort, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hPort == INVALID_HANDLE_VALUE) 
    {
        printf("Error opening serial port, is another app using it?\n");
        return 0;
    }

    //Get current settings 
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hPort, &dcbSerialParams)) 
    {
        printf("Error getting COM port state\n");
        CloseHandle(hPort);
        return 0;
    }

    //Set comm parameters  
    dcbSerialParams.BaudRate = 9600;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hPort, &dcbSerialParams))
    {
        printf("Error setting device state\n");
        CloseHandle(hPort);
        return 0;
    }

    //Set timeouts though these are probably the defaults anyway
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hPort, &timeouts);
    
    return 1;
}

//Add given byte to send buffer
static int AddByteToBuffer(uint8_t Byte)
{
    if (NextBufferSpot >= BUF_SIZE)
        return 0;
    Buffer[NextBufferSpot++] = Byte;
    return 1;
}

//Send the buffer given the number of 8 byte packets in it, one packet at a time,
//ending the packet with 0x0a and waiting between packets SendDelay milliseconds
//(so this may take awhile to return)
static int SendBuffer(int NumPackets, const char *DebugName)
{
    uint8_t CR = 0x0a;
    DWORD Written;
    char DebugString[256];
    for (int i = 0; i < NumPackets; i++)
    {
        WriteFile(hPort, &Buffer[i * 8], 8, &Written, nullptr);
        WriteFile(hPort, &CR, 1, &Written, nullptr);

        //Debug output, show what was sent in hex 
        int j = i * 8;
        sprintf_s(DebugString, 256, "%s: %02X %02X %02X %02X %02X %02X %02X %02X 0A\n", 
            DebugName, Buffer[j], Buffer[j + 1], Buffer[j + 2], Buffer[j + 3], 
            Buffer[j + 4], Buffer[j + 5], Buffer[j + 6], Buffer[j + 7]);
        printf(DebugString);

        Sleep(SEND_DELAY_MS);
    }

    return 1;
}

//Set the PS field and stream it out, max 8 characters not including terminating 0, 
//any more won't be sent. Automatically pads with spaces if less than 8, per the protocol.
static int SetPS(const char* PSString)
{
    //Copy into padded string (not zero terminated) and fill excess with spaces
    char PaddedString[8] = { 0 };
    size_t len = strlen(PSString);
    for (int i = 0; i < 8; i++)
    {
        if (i < len)
            PaddedString[i] = PSString[i];
        else
            PaddedString[i] = ' ';
    }

    //reset the buffer
    NextBufferSpot = 0;

    //Assemble the four PS packets into the buffer
    for (int i = 0; i < 4; i++)
    {
        //2 bytes: high and low PI bytes
        AddByteToBuffer(PI >> 8);    //high byte
        AddByteToBuffer(PI & 0x00FF);//low

        //2 bytes that are bitfields, first 3 bits are packet type = 0 0 0 (PS?), then two 
        //bits 0 0, 6 bits PTY, two bits 0 1, then 3 bits sequence number
        uint16_t Bits16 = (PTY << 5) | (1 << 3) | i;
        AddByteToBuffer(Bits16 >> 8);
        AddByteToBuffer(Bits16 & 0xFF);

        //4 bytes payload but first two are always 0 (or otherwise ignored), then 2 
        //characters of the PS string at a time. 
        AddByteToBuffer(0);
        AddByteToBuffer(0);
        AddByteToBuffer(PaddedString[i * 2]);
        AddByteToBuffer(PaddedString[i * 2 + 1]);
    }
    SendBuffer(4, "PS");

    return 1;
}

//Set the RT string and stream it out, max 64 characters not including terminating 0 
//(any more won't be sent). 
static int SetRT(const char* PSString)
{
    //Copy into padded string (not zero terminated) and fill excess with spaces
    char PaddedString[64] = { 0 };
    size_t len = strlen(PSString);
    for (int i = 0; i < 64; i++)
    {
        if (i < len)
            PaddedString[i] = PSString[i];
        else
            PaddedString[i] = ' ';
    }

    //reset the buffer
    NextBufferSpot = 0;

    //Assemble the 16 packets into the buffer
    for (int i = 0; i < 16; i++)
    {
        //2 bytes: high and low PI bytes
        AddByteToBuffer(PI >> 8);    
        AddByteToBuffer(PI & 0x00FF);

        //2 bytes that are bitfields, first 4 bits packet type = 0 0 1 0 = RT, then two  
        // zero bits (to indicate we're using version A format of the RT message, and 
        // we don't provide traffic information), 5 bits PTY, 1 bit 0 (not requesting 
        // screen clear which doesn't seem to do anything on many receivers), then 4 bits 
        // sequence number
        uint16_t Bits16 = (1 << 13) | (PTY << 5) | i;
        AddByteToBuffer(Bits16 >> 8);
        AddByteToBuffer(Bits16 & 0xFF);

        //4 bytes payload that are 4 characters of the RT string at a time 
        AddByteToBuffer(PaddedString[i * 4]);
        AddByteToBuffer(PaddedString[i * 4 + 1]);
        AddByteToBuffer(PaddedString[i * 4 + 2]);
        AddByteToBuffer(PaddedString[i * 4 + 3]);
    }
    SendBuffer(16, "RT");

    return 1;
}

int main() 
{
    if (!Initialize("COM3", 0x1234, 1)) 
        return 1;
    
    while (!_kbhit())  
    {   
        SetPS("ROCK 97");
        SetRT("Artist Name - Song Title"); 
    }
    CloseHandle(hPort);

    return 0;
}
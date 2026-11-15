# GD-2015 and GD-2025 FM Transmitter RDS Driver
This Windows console app listens for real-time exported RDS data from RadioDJ or possibly other FM automation software and sets the RDS fields on the 
China Broadcast GD-2015 series transmitter. You can also run it with default RDS settings on the command line to set static information and save it
to automatically boot on transmitter power-up. 

You can set PI, PS, PTY and RT RBDS/RDS fields that will display on car FM radio screens and advanced receivers. By default the RT field which can
be 64 characters will split into two 32 character timeshared transmissions because many car radios just clip it if it goes past the screen width.
That and other options are shown at the top of the code file. 

Setting the PS field to "%%%%" will scroll RT text (one word at a time) into the PS field. This is done by some stations in North America because
older FM car radios only showed the PS field so they wanted a way to show artist and title information, but it violates the official RDS
specification (PS field is expected to be set once and not change) so might not be a good idea elsewhere. Check your local radio stations if they
scroll PS. But usually PS is for the station ID. 

You'll need a USB to serial (RS-232) adapter which you plug into the transmitter directly (no null model adapter needed). Find out what COM port
is assigned to but the default is COM3. 

To run in real-time mode, set the transmitter RDS page to "UNSAVE" and select it, which will make it listen to the serial port. Launch this console app
and it will start listening on port 21100 by default (see main() function for command line options). On RadioDJ choose the Metadata Export button
along the bottom, Network tab, choose the IP the machine that's plugged into the transmitter is at (127.0.0.1 if the same computer as RadioDJ). 
Leave UDP-IP protocol, browse to the Medadata Export folder for the Text Template.txt template file and select that under Template File.
Leave the default port 21100, click enable, and click the green plus sign. That adds it to the list of ports it exports too. Click Save and
it will remember it. 

After that you can power on the transmitter, start RadioDJ and start this app in any order and they should all connect. 

To set one-time static RDS data and save it in the transmitter (e.g. just station type, ID and default RT text) run this standalone with command
line parameters (see the main() function for expected order) or change the defaults in the code, connect it to the transmitter, choose
the RDS page "UNSAVE" mode until it's displayed on your receiver properly, select "WRITE" and "SAVE" on the RDS transmitter page and leave it on
"SAVE." Next time it powers up it will pull the saved data and ignore the RS-232 port. Select "UNSAVE" to return the transmitter to listen mode.

To compile with Visual Studio, make a new "Console app" hello world project then replace the main file with this one.


/*
 * serialCOM.cpp - Serial port configuration and communication (macOS version)
 *
 * Copyright (C) 2008-2023 Comer352L
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "serialCOM.h"


serialCOM::serialCOM()
{
	fd = -1;
	portisopen = false;
	breakset = false;
	currentportname = "";
	memset(&oldtio, 0, sizeof(oldtio));
	settingssaved = false;
	custombaudrate = 0;
}


serialCOM::~serialCOM()
{
	if (portisopen) ClosePort();
}


std::vector<std::string> serialCOM::GetAvailablePorts()
{
	std::vector<std::string> portlist(0);
	int testfd = -1;				// file descriptor for tested device files
	char ffn[256] = "";				// full filename incl. path
	DIR *dp = NULL;
	struct dirent *fp = NULL;
	// CHECK DEVICE FILES:
	/* NOTE: macOS creates two device files for each serial port:
	 *       /dev/tty.* (dial-in, open() blocks until DCD is asserted) and /dev/cu.* (call-out)
	 *       => only the call-out devices are suitable for us
	 *       Examples: cu.usbserial-XXXXXXXX (FTDI), cu.usbmodemXXXX (CDC-ACM), cu.wchusbserialXXXX (CH34x), cu.SLAB_USBtoUART (CP210x)
	 */
	dp = opendir ("/dev");
	if (dp != NULL)
	{
		do {
			fp = readdir (dp);	// get next file in directory
			if (fp != NULL)
			{
				if (!strncmp(fp->d_name,"cu.",3) && (strlen(fp->d_name) < (sizeof(ffn) - 5)))	// if filename starts with "cu."
				{
					// CONSTRUCT FULL FILENAME:
					strcpy(ffn, "/dev/");		// (replaces old string)
					strcat(ffn, fp->d_name);
					testfd = -1;
					testfd = open(ffn, O_RDWR | O_NOCTTY | O_NONBLOCK);
					if (!(testfd == -1))
					{
						// FIND OUT, IF FILE IS A TTY:
						if (isatty(testfd))
							portlist.push_back(ffn);
						close(testfd);
					}
				}
			}
		} while (fp != NULL);
		closedir (dp);
	}
#ifdef __SERIALCOM_DEBUG__
	else
		std::cout << "serialCOM::GetAvailablePorts():   opendir(''/dev'') failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	return portlist;
}


bool serialCOM::IsOpen()
{
	if (portisopen && (fd >= 0))
		return true;
	else
		return false;
}


std::string serialCOM::GetPortname()
{
	return currentportname;
}


bool serialCOM::GetPortSettings(double *baudrate, unsigned short *databits, char *parity, float *stopbits)
{
	/* NOTE: NULL-pointer arguments allowed ! */
	bool settingsvalid = true;
	unsigned short nrofdatabits = 0;
	struct termios currenttio;
	memset(&currenttio, 0, sizeof(currenttio));
	if (!portisopen) return false;
	// Query current settings:
	if (tcgetattr(fd, &currenttio) == -1)
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::GetPortSettings():   tcgetattr(...) failed with error " << errno << " " << strerror(errno)<< "\n";
#endif
		return false;
	}
	// BAUD RATE:
	if (baudrate)
	{
		if (custombaudrate > 0)
		{
			/* NOTE: the IOSSIOSPEED-ioctl bypasses the termios struct, so tcgetattr()
			 *       still reports the (standard) baud rate we have set before */
			*baudrate = custombaudrate;
		}
		else
		{
			speed_t ispeed = cfgetispeed(&currenttio);
			speed_t ospeed = cfgetospeed(&currenttio);
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::GetPortSettings(): baud rates in struct termios:\n";
			std::cout << "   ispeed: " << ispeed << '\n';
			std::cout << "   ospeed: " << ospeed << '\n';
#endif
			if ((ispeed != ospeed) && (ispeed != 0))	// NOTE: ispeed=0 means "same as ospeed"
			{
				settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
				std::cout << "serialCOM::GetPortSettings():   ERROR:   different baud rates for transmitting and receiving detected\n";
#endif
			}
			else if (ospeed == B0)	// B0 is not allowed (=> Windows compatibility)
			{
				*baudrate = 0;
			}
			else
			{
				// NOTE: on macOS, the Bxxx constants are equal to the baud rate values (except B134 = 134.5 baud)
				*baudrate = ospeed;
				for (unsigned int k=0; k<(sizeof(std_baudrates)/sizeof(std_baudrate)); k++)
				{
					if (std_baudrates[k].constant == ospeed)
					{
						*baudrate = std_baudrates[k].value;
						break;
					}
				}
			}
		}
	}
	// DATABITS:
	switch (currenttio.c_cflag & CSIZE)
	{
		case CS8:
			nrofdatabits = 8;
			break;
		case CS7:
			nrofdatabits = 7;
			break;
		case CS6:
			nrofdatabits = 6;
			break;
		case CS5:
			nrofdatabits = 5;
			break;
	}
	if (databits)
	{
		if (nrofdatabits)
			*databits = nrofdatabits;
		else
		{
			settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::GetPortSettings():   error: unknown number of databits\n";
#endif
		}
	}
	// PARITY:
	if (parity)
	{
		if (!(currenttio.c_cflag & PARENB))	// if parity-flag is not set
			*parity='N';
		else if (currenttio.c_cflag & PARODD)	// if odd-parity-flag is set
			*parity='O';
		else
			*parity='E';
		// NOTE: mark/space parity is not supported on macOS
	}
	// STOPBITS:
	if (stopbits)
	{
		if (!(currenttio.c_cflag & CSTOPB))
			*stopbits = 1;
		else
		{
			if (nrofdatabits == 5)
			{
				*stopbits = 1.5;
			}
			else if (nrofdatabits > 5)
			{
				*stopbits = 2;
			}
			else
			{
				settingsvalid = false;
				*stopbits = 0;
			}
		}
	}
	// RETURN SUCCESS:
	return settingsvalid;
}


bool serialCOM::SetPortSettings(double baudrate, unsigned short databits, char parity, float stopbits)
{
	bool isStdBaud = true;
	bool settingsvalid = true;
	speed_t newbaudrate = 0;
	struct termios newtio;
	memset(&newtio, 0, sizeof(newtio));
	if (!portisopen) return false;
	// SET CONTROL OPTIONS:
	newtio.c_cflag = (CREAD | CLOCAL);
	// BAUD RATE:
	if (baudrate <= 0)
	{
		settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
		if (baudrate == 0)
			std::cout << "serialCOM::SetPortSettings:   error: illegal baud rate - 0 baud not possible\n";
		else
			std::cout << "serialCOM::SetPortSettings:   error: illegal baud rate - baud rate must be > 0\n";
#endif
	}
	else
	{
		isStdBaud = GetStdBaudRateDCBConst(baudrate, &newbaudrate);
		if (!isStdBaud)
		{
			/* NOTE: Non-standard baud rates are set with the IOSSIOSPEED-ioctl after the termios
			 *       settings have been applied. If the driver doesn't support the requested
			 *       baud rate, the nearest standard baud rate set via termios is kept.
			 */
			newbaudrate = GetNearestStdBaudrate(baudrate);
		}
		// else: GetStdBaudRateDCBConst(...) has set newbaudrate to the standard baud rate constant
	}
	// DATABITS:
	switch (databits)
	{
		case 8:
			newtio.c_cflag |= CS8;
			break;
		case 7:
			newtio.c_cflag |= CS7;
			break;
		case 6:
			newtio.c_cflag |= CS6;
			break;
		case 5:
			newtio.c_cflag |= CS5;
			break;
		default:
			settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::SetPortSettings():  error: illegal number of databits\n";
#endif
			break;
	}
	// PARITY:
	if (parity == 'N')
	{
		newtio.c_cflag &= ~PARENB;	// deactivate parity (not really necessary, because c_cflag is clean)
	}
	else if (parity == 'E')
	{
		newtio.c_cflag |= PARENB;	// activate parity
		newtio.c_cflag &= ~PARODD;	// deactivate odd parity (not really necessary, because c_cflag is clean)
	}
	else if (parity == 'O')
	{
		newtio.c_cflag |= PARENB;	// activate parity
		newtio.c_cflag |= PARODD;	// activate odd parity
	}
	else	// NOTE: mark/space parity is not supported on macOS
	{
		settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetPortSettings():  error: illegal parity option\n";
#endif
	}
	// STOPBITS:
	if (stopbits == 1)
	{
		newtio.c_cflag &= ~CSTOPB;	//  (not really necessary, because c_cflag is clean)
	}
	else if (stopbits == 1.5)
	{
		if (databits == 5)
			newtio.c_cflag |= CSTOPB;
		else
		{
			settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::SetPortSettings():   error: invalid value for parameter 'stopbits': 1.5 stopbits only allowed in combination with 5 databits\n";
#endif
		}
	}
	else if (stopbits == 2)
	{
		if (databits != 5)
			newtio.c_cflag |= CSTOPB;
		else
		{
			settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::SetPortSettings():   error: invalid value for parameter 'stopbits': 2 stopbits not allowed in combination with 5 databits\n";
#endif
		}
	}
	else
	{
		settingsvalid = false;
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetPortSettings():   error: invalid value for parameter 'stopbits'" << '\n';
#endif
	}
	// LINE OPTIONS (LOCAL OPTIONS): raw input
	newtio.c_lflag = NOFLSH;
	// INPUT OPTIONS:
	newtio.c_iflag = IGNPAR | IGNBRK;
	// OUTPUT OPTIONS:
	newtio.c_oflag &= ~OPOST;  // => raw output - all other option bits will be ignored
	// Timeout settings (for input):
	newtio.c_cc[VMIN] = 0;
	newtio.c_cc[VTIME] = 0;
	// SET NEW PORT SETTING:
	if (!settingsvalid)	// apply new settings only if they are all valid !
		return false;
	if (cfsetspeed(&newtio, newbaudrate) == -1)
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetPortSettings():   cfsetspeed(...) failed with error " << errno << " " << strerror(errno)<< "\n";
#endif
		return false;
	}
	if (tcsetattr(fd, TCSANOW, &newtio) == -1)
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetPortSettings():   tcsetattr(...) failed with error " << errno << " " << strerror(errno)<< "\n";
#endif
		return false;
	}
	/* NOTE: tcsetattr() resets baud rates which have been set with the IOSSIOSPEED-ioctl */
	custombaudrate = 0;
	if (!isStdBaud)
	{
		speed_t speed = static_cast<speed_t>(round(baudrate));
		if (ioctl(fd, IOSSIOSPEED, &speed) != -1)
			custombaudrate = speed;
#ifdef __SERIALCOM_DEBUG__
		else
			std::cout << "serialCOM::SetPortSettings():   ioctl(..., IOSSIOSPEED, ...) failed with error " << errno << " " << strerror(errno) << "\n => using nearest standard baud rate instead\n";
#endif
	}
	return true;
}


bool serialCOM::OpenPort(std::string portname)
{
	int confirm = -1;	// -1=error
	if (portisopen) return false;	// if port is already open => cancel and return "false"
	memset(&oldtio, 0, sizeof(oldtio));
	custombaudrate = 0;
	// OPEN PORT:
	fd = open(portname.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	/* 		O_RDWR	 = read/write access;
	*		O_NOCTTY = ignore control characters
	*		O_NONBLOCK = ignore DCD-line		*/
	if (!(fd < 0))	// if port is now open
	{
		portisopen = true;
		currentportname = portname;
		confirm = ioctl(fd, TIOCEXCL);	// LOCK DEVICE
#ifdef __SERIALCOM_DEBUG__
		if (confirm == -1)
			std::cout << "serialCOM::OpenPort():   ioctl(..., TIOCEXCL) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		confirm = fcntl(fd, F_SETFL, O_NONBLOCK);	// function "read" shall return "0" if no data available
		if (confirm == -1)
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::OpenPort():   fcntl(...) failed with error " << errno << "\n";
#endif
			confirm = ClosePort();
#ifdef __SERIALCOM_DEBUG__
			if (!confirm)
				std::cout << "serialCOM::OpenPort():   port couldn't be closed after error during opening process\n";
#endif
			return false;
		}
		// SAVE SETTINGS:
		confirm = tcgetattr(fd, &oldtio);
		if (confirm == -1)
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::OpenPort():   tcgetattr(...) failed with error " << errno << "\n";
#endif
			settingssaved = false;
		}
		else
			settingssaved = true;
		// REQUEST LOW LATENCY BEHAVIOUR:
		unsigned long latency_us = 1;
		confirm = ioctl(fd, IOSSDATALAT, &latency_us);	// receive latency in microseconds
		/* NOTE: drivers are allowed to ignore this, so a failure is not critical */
#ifdef __SERIALCOM_DEBUG__
		if (confirm == -1)
			std::cout << "serialCOM::OpenPort():   ioctl(..., IOSSDATALAT, ...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		breakset = false;
		// CLEAR HARDWARE BUFFERS:
		confirm = tcflush(fd, TCIOFLUSH);
		if (confirm == -1)
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::OpenPort():   tcflush(..., TCIOFLUSH) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
			confirm = ClosePort();
#ifdef __SERIALCOM_DEBUG__
			if (!confirm)
				std::cout << "serialCOM::OpenPort():   port couldn't be closed after error during opening process\n";
#endif
			return false;
		}
		// CONFIGURE COMMUNICATION, SET STANDARD PORT-SETTINGS
		if (!SetPortSettings(9600, 8, 'N', 1 ))
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::OpenPort():   Couldn't set standard port settings with SetPortSettings() !\n";
#endif
			confirm = ClosePort();
#ifdef __SERIALCOM_DEBUG__
			if (!confirm)
				std::cout << "serialCOM::OpenPort():   port couldn't be closed after error during opening process\n";
#endif
			return false;
			/* NOTE: SetPortSettings not only changes the 4 communication parameters.
			         It configures additional parameters (like control characters, timeouts, ...) which are
			         are important to ensure proper communication behavior !
			 */
		}
		// SET CONTROL LINES (DTR+RTS) TO STANDARD VALUES:
		confirm = SetControlLines(true, true);
#ifdef __SERIALCOM_DEBUG__
		if (!confirm)
			std::cout << "serialCOM::OpenPort():   Warning: couldn't set RTS+DTS control lines to standard values\n";
#endif
		/* NOTE: Call SetControlLines AFTER SetPortSettings, because drivers can
		 * change DTS+RTS when new baud rate / databits / parity / stopbits,
		 * especially at the first time after opening the port !		*/
		return true;
	}
	else
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::OpenPort():   open(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::ClosePort()
{
	int confirm = -1;	// -1=error
	if (!portisopen) return false;
	// CLEAR BREAK:
	confirm = ioctl(fd, TIOCCBRK);    // break OFF
	if (confirm != -1)
		breakset = false;
#ifdef __SERIALCOM_DEBUG__
	else
		std::cout << "serialCOM::ClosePort():   ioctl(..., TIOCCBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	// CLEAR HARDWARE BUFFERS:
	confirm = tcflush(fd, TCIOFLUSH);	// clear buffers (input and output)
#ifdef __SERIALCOM_DEBUG__
	if (confirm == -1)
		std::cout << "serialCOM::ClosePort():   tcflush(..., TCIOFLUSH) failed with error " << errno << "\n";
#endif
	// RESTORE PORT SETTINGS:
	if (settingssaved)
	{
		confirm = tcsetattr(fd, TCSANOW, &oldtio);	// restore the old port settings
#ifdef __SERIALCOM_DEBUG__
	if (confirm == -1)
		std::cout << "serialCOM::ClosePort():   tcsetattr(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	}
	confirm = ioctl(fd, TIOCNXCL);	// unlock device
#ifdef __SERIALCOM_DEBUG__
	if (confirm == -1)
		std::cout << "serialCOM::ClosePort():   ioctl(..., TIOCNXCL) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	// CLOSE PORT:
	confirm = close(fd);
	// CLEAN UP, RETURN VALUE:
	if (confirm==0)
	{
		fd = -1;
		portisopen = false;
		breakset = false;
		currentportname = "";
		custombaudrate = 0;
		return true;
	}
	else
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::ClosePort():   close(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::Write(std::vector<char> data)
{
	return Write(&data.at(0), data.size());
}


bool serialCOM::Write(char *data, unsigned int datalen)
{
	int confirm = -1;
	ssize_t nrofbyteswritten = 0;
	if (!portisopen) return false;
	if (breakset)
	{
		confirm = ioctl(fd, TIOCCBRK);	// break OFF
		if (confirm != -1)
			breakset = false;
		else
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::Write():   ioctl(..., TIOCCBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
			return false;
		}
	}
	// SEND DATA:
	nrofbyteswritten = write(fd, data, datalen);
	// WAIT UNTIL ALL BYTES ARE TRANSMITTED:
	confirm = tcdrain(fd);
	// RETURN VALUE:
	if ((nrofbyteswritten == static_cast<ssize_t>(datalen)) && (confirm != -1))
		return true;
	else
	{
#ifdef __SERIALCOM_DEBUG__
		if (nrofbyteswritten != static_cast<ssize_t>(datalen))
			std::cout << "serialCOM::Write():   write(...) failed with error " << errno << " " << strerror(errno) << "\n";
		if (confirm == -1)
			std::cout << "serialCOM::Write():   tcdrain(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::Read(unsigned int minbytes, unsigned int maxbytes, unsigned int timeout, std::vector<char> *data)
{
	if (!portisopen || (minbytes > maxbytes) || (maxbytes > INT_MAX))
		return false;
	unsigned int rdatalen = 0;
	char *rdata = (char*) malloc(maxbytes);
	if (rdata == NULL) return false;
	bool ok = Read(minbytes, maxbytes, timeout, rdata, &rdatalen);
	if (ok)	data->assign(rdata, rdata+rdatalen);
	free(rdata);
	return ok;
}


bool serialCOM::Read(unsigned int minbytes, unsigned int maxbytes, unsigned int timeout, char *data, unsigned int *nrofbytesread)
{
	ssize_t ret;
	*nrofbytesread = 0;
	if (!portisopen || (minbytes > maxbytes) || (maxbytes > INT_MAX))
		return false;

	if (!minbytes)
	{
		ret = read(fd, data, maxbytes);
		if ((ret >= 0) && (ret <= static_cast<ssize_t>(maxbytes)))
		{
			*nrofbytesread = static_cast<unsigned int>(ret);
			return true;
		}
		else if ((ret == -1) && (errno == EAGAIN))	// non-blocking mode: no data available
		{
			return true;
		}
		else
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::Read():   read(..) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
			return false;
		}
	}
	else
	{
		unsigned int rb_total = 0;
		unsigned int t_remaining_ms = 0;
		struct timespec t_current;
		struct timespec t_start;
		fd_set input;
		struct timeval sel_timeout;
		int n = 0;

		if (timeout > 0)
		{
			if (clock_gettime(CLOCK_MONOTONIC, &t_start) != 0)	// returns -1 on error, 0 on success
			{
#ifdef __SERIALCOM_DEBUG__
				std::cout << "serialCOM::Read():   clock_gettime(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
				return false;
			}
			t_remaining_ms = timeout;
		}
		do
		{
			FD_ZERO(&input);
			FD_SET(fd, &input);
			// Wait for data:
			if (timeout > 0)
			{
				sel_timeout.tv_sec  = t_remaining_ms / 1000;
				sel_timeout.tv_usec = (t_remaining_ms % 1000)*1000;
				n = select(fd+1, &input, NULL, NULL, &sel_timeout);
			}
			else // wait indefinitely
				n = select(fd+1, &input, NULL, NULL, NULL);
			/* NOTE: on macOS, select() doesn't modify timeout to reflect the time not slept */
			// See if there was an error, read available data:
			if (n < 0)
			{
#ifdef __SERIALCOM_DEBUG__
				std::cout << "serialCOM::Read():   select(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
				return false;
			}
			else if (n == 0)
			{
#ifdef __SERIALCOM_DEBUG__
				std::cout << "serialCOM::Read():   TIMEOUT\n";
#endif
				break;
			}
			else if (FD_ISSET(fd, &input))
			{
				// Read available data:
				ret = read(fd, data+rb_total, maxbytes-rb_total);	// NOTE: returns immediately
				if ((ret >= 0) && (ret <= static_cast<ssize_t>(maxbytes-rb_total)))
					rb_total += ret;
				else if (!((ret == -1) && (errno == EAGAIN)))
				{
					*nrofbytesread = 0;
#ifdef __SERIALCOM_DEBUG__
					std::cout << "serialCOM::Read():   read(..) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
					return false;
				}
			}
			if (timeout > 0)
			{
				// Get current time, calculate remaining time:
				if (clock_gettime(CLOCK_MONOTONIC, &t_current) != 0)	// returns -1 on error, 0 on success
				{
#ifdef __SERIALCOM_DEBUG__
					std::cout << "serialCOM::Read():   clock_gettime(...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
					return false;
				}
				t_remaining_ms = timeout - ((t_current.tv_sec*1000 + t_current.tv_nsec/1000000) - (t_start.tv_sec*1000 + t_start.tv_nsec/1000000)); // NOTE: overflow possible !
			}
		} while (((timeout == 0) || ((t_remaining_ms > 0) && (t_remaining_ms <= timeout))) && (rb_total < minbytes)); // NOTE: 2nd check of t_remaining_ms: for detecting overflow
		*nrofbytesread = static_cast<unsigned int>(rb_total);
		return true;
	}
	/* NOTE: - we always return the received bytes even if we received less than minbytes (timeout)
	         - return value indicates error but not a timeout (can be checked by comparing minbytes and nrofbytesread) */
}


bool serialCOM::ClearSendBuffer()
{
	int cvTF = -1;
	if (portisopen)
		cvTF = tcflush(fd, TCOFLUSH);
	if (cvTF != -1)
		return true;
	else
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::ClearSendBuffer(...):   tcflush(..., TCOFLUSH) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::ClearReceiveBuffer()
{
	int cvTF = -1;
	if (portisopen)
		cvTF = tcflush(fd, TCIFLUSH);
	if (cvTF != -1)
		return true;
	else
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::ClearReceiveBuffer(...):   tcflush(..., TCIFLUSH) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::SendBreak(unsigned int duration_ms)
{
	int confirmSB = -1;	// 0 or -1
	if ((!portisopen) || (duration_ms < 1) || (duration_ms >= 32767))
		return false;
	breakset = true;
	/* NOTE: on macOS, tcsendbreak() ignores the duration argument and always sends a 0.4s break
	 *       => we have to do the timing on our own					*/
	confirmSB = ioctl(fd, TIOCSBRK);	// break ON
	if (confirmSB != -1)
	{
		usleep(1000*duration_ms);
		confirmSB = ioctl(fd, TIOCCBRK);	// break OFF
		if (confirmSB == -1)
		{
#ifdef __SERIALCOM_DEBUG__
			std::cout << "serialCOM::SendBreak(...):   ioctl(..., TIOCCBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
			return false;	// WITH breakset STILL TRUE !
		}
	}
#ifdef __SERIALCOM_DEBUG__
	else
		std::cout << "serialCOM::SendBreak(...):   ioctl(..., TIOCSBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	breakset = false;
	if (confirmSB == -1)
		return false;
	return true;
}


bool serialCOM::SetBreak()
{
	int confirmIOCTL = -1;
	if (!portisopen) return false;
	confirmIOCTL = ioctl(fd, TIOCSBRK);	// break ON
	if (confirmIOCTL == -1)
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetBreak(...):   ioctl(..., TIOCSBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
	breakset = true;
	return true;
}


bool serialCOM::ClearBreak()
{
	int confirmIOCTL = -1;
	if (!portisopen) return false;
	confirmIOCTL = ioctl(fd, TIOCCBRK);    // break OFF
	if (confirmIOCTL == -1)
	{
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::ClearBreak(...):   ioctl(..., TIOCCBRK) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
	breakset = false;
	return true;
}


bool serialCOM::BreakIsSet()
{
	return breakset;
}


bool serialCOM::GetNrOfBytesAvailable(unsigned int *nbytes)
{
	int bytes = 0;
	if (!portisopen) return false;
	if (ioctl(fd, FIONREAD, &bytes) != -1)
	{
		*nbytes = static_cast<unsigned int>(bytes);
		return true;
	}
	else
	{
		*nbytes = 0;
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::GetNrOfBytesAvailable(...):   ioctl(..., FIONREAD, ...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
		return false;
	}
}


bool serialCOM::SetControlLines(bool DTR, bool RTS)
{
	int linestatus = 0;
	int retIOCTL = -1;
	if (!portisopen) return false;
	retIOCTL = ioctl(fd, TIOCMGET, &linestatus);
	if (retIOCTL == -1)
	{
		linestatus |= TIOCM_ST; // set seconary TX-line (only DB25) to 1/HIGH = idle state;
#ifdef __SERIALCOM_DEBUG__
		std::cout << "serialCOM::SetControlLines():   ioctl(..., TIOCMGET, ...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	}
	if (DTR)
		linestatus |= TIOCM_DTR;	// "Ready"
	else
		linestatus &= ~TIOCM_DTR;	// "NOT Ready"
	if (RTS)
		linestatus |= TIOCM_RTS;	// "Request"
	else
		linestatus &= ~TIOCM_RTS;	// "NO Request"
	/* NOTE: lines are inverted. Set flag means line=0/low/"space" */
	retIOCTL = ioctl(fd, TIOCMSET, &linestatus);
#ifdef __SERIALCOM_DEBUG__
	if (retIOCTL == -1)
		std::cout << "serialCOM::SetControlLines():   ioctl(..., TIOCMSET, ...) failed with error " << errno << " " << strerror(errno) << "\n";
#endif
	return (retIOCTL != -1);
}

// PRIVATE

struct serialCOM::std_baudrate serialCOM::std_baudrates[] = {
	{50, B50},
	{75, B75},
	{110, B110},
	{134.5, B134},
	{150, B150},
	{200, B200},
	{300, B300},
	{600, B600},
	{1200, B1200},
	{1800, B1800},
	{2400, B2400},
	{4800, B4800},
	{9600, B9600},
	{19200, B19200},
	{38400, B38400},
	{57600, B57600},
	{115200, B115200},
#ifdef B230400
	{230400, B230400},
#endif
};
// B0 not used, because of Windows compatibility


bool serialCOM::GetStdBaudRateDCBConst(double baudrate, speed_t *DCBbaudconst)
{
	for (unsigned int k=0; k<(sizeof(std_baudrates)/sizeof(std_baudrate)); k++)
	{
		if (std_baudrates[k].value == baudrate)
		{
			*DCBbaudconst = std_baudrates[k].constant;
			return true;
		}
	}
	return false;
}


speed_t serialCOM::GetNearestStdBaudrate(double selBaudrate)
{
	// Get nearest standard baud rate:
	speed_t nearestBaudrate = 0;

	unsigned int stdbaudelements = (sizeof(std_baudrates)/sizeof(std_baudrate));

	if (selBaudrate <= std_baudrates[0].value)
	{
		nearestBaudrate=std_baudrates[0].constant;
	}
	else if (selBaudrate >= std_baudrates[stdbaudelements-1].value)
	{
		nearestBaudrate=std_baudrates[stdbaudelements-1].constant;
	}
	else
	{
		double q2=0;
		double q1=0;
		for (unsigned int b=1; b<stdbaudelements; b++)
		{
			q2 = std_baudrates[b].value / selBaudrate;
			if (q2 >= 1)
			{
				// br[b-1] < baudrate < br[b]:
				q1 = std_baudrates[b-1].value / selBaudrate;
				// compare relative baud rate deviation, select baud rate:
				if ((q2-1) < (1-q1))
					nearestBaudrate=std_baudrates[b].constant;
				else
					nearestBaudrate=std_baudrates[b-1].constant;
				break;
			}
		}
	}
	return nearestBaudrate;
}


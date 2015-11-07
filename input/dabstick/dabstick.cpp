#
/*
 *    Copyright (C) 2010, 2011, 2012, 2013
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Programming
 *
 *    This file is part of the SDR-J.
 *    Many of the ideas as implemented in SDR-J are derived from
 *    other work, made available through the GNU general Public License. 
 *    All copyrights of the original authors are recognized.
 *
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * 	This particular driver is a very simple wrapper around the
 * 	librtlsdr.  In order to keep things simple, we dynamically
 * 	load the dll (or .so). The librtlsdr is osmocom software and all rights
 * 	are greatly acknowledged
 */


#include	<QThread>
#include	"rtl-sdr.h"
#include	"dabstick.h"

#ifdef	__MINGW32__
#define	GETPROCADDRESS	GetProcAddress
#else
#define	GETPROCADDRESS	dlsym
#endif

#define	READLEN_DEFAULT	8192
//
//	For the callback, we do need some environment which
//	is passed through the ctx parameter
//
//	This is the user-side call back function
//	ctx is the calling task
static
void	RTLSDRCallBack (uint8_t *buf, uint32_t len, void *ctx) {
dabStick	*theStick	= (dabStick *)ctx;
int32_t	tmp;

	if ((theStick == NULL) || (len != READLEN_DEFAULT))
	   return;

	tmp = theStick -> _I_Buffer -> putDataIntoBuffer (buf, len);
	if ((len - tmp) > 0)
	   theStick	-> sampleCounter += len - tmp;
}
//
//	for handling the events in libusb, we need a controlthread
//	whose sole purpose is to process the rtlsdr_read_async function
//	from the lib.
class	dll_driver : public QThread {
private:
	dabStick	*theStick;
public:

	dll_driver (dabStick *d) {
	theStick	= d;
	start ();
	}

	~dll_driver (void) {
	}

private:
virtual void	run (void) {
	(theStick -> rtlsdr_read_async) (theStick -> device,
	                          (rtlsdr_read_async_cb_t)&RTLSDRCallBack,
	                          (void *)theStick,
	                          0,
	                          READLEN_DEFAULT);
	}
};
//
//	Our wrapper is a simple classs
	dabStick::dabStick (QSettings *s, bool full, bool *success) {
int16_t	deviceCount;
int32_t	r;
int16_t	deviceIndex;
int16_t	i;
QString	h;
int	k;

	dabSettings		= s;
	this	-> myFrame	= new QFrame (NULL);
	setupUi (this -> myFrame);
	this	-> myFrame	-> show ();

	dabSettings	-> beginGroup ("dabstick");
	h	= dabSettings	-> value ("dabRate", 1536). toString ();
	k	= rateSelector -> findText (h);
	if (k != -1)
	   rateSelector -> setCurrentIndex (k);
	inputRate	=  Khz (rateSelector -> currentText (). toInt ());
	dabSettings	-> endGroup ();
//
//	for the mini we only allow selection of this value,
//	i.e. kill all others
	if (!full) {
	   while (rateSelector -> count () > 0)
	      rateSelector -> removeItem (rateSelector -> currentIndex ());
	   rateSelector	-> addItem (QString::number (inputRate / Khz (1)));
	}
	  
	*success			= false;	// just the default
	libraryLoaded			= false;
	open				= false;
	_I_Buffer			= NULL;
	this	-> sampleCounter	= 0;
	this	-> vfoOffset		= 0;
	gains				= NULL;

#ifdef	__MINGW32__
	const char *libraryString = "rtlsdr.dll";
	Handle		= LoadLibrary ((wchar_t *)L"rtlsdr.dll");
#else
	const char *libraryString = "librtlsdr.so";
	Handle		= dlopen ("librtlsdr.so", RTLD_NOW);
#endif

	if (Handle == NULL) {
	   fprintf (stderr, "failed to open %s\n", libraryString);
	   return;
	}

	libraryLoaded	= true;
	if (!load_rtlFunctions ())
	   goto err;

//
//	Ok, from here we have the library functions accessible
	deviceCount 		= this -> rtlsdr_get_device_count ();
	if (deviceCount == 0) {
	   fprintf (stderr, "No devices found\n");
	   return;
	}

	deviceIndex = 0;	// default
	if (deviceCount > 1) {
	   dongleSelector	= new dongleSelect ();
	   for (deviceIndex = 0; deviceIndex < deviceCount; deviceIndex ++) {
	      dongleSelector ->
	           addtoDongleList (rtlsdr_get_device_name (deviceIndex));
	   }
	   deviceIndex = dongleSelector -> QDialog::exec ();
	   delete dongleSelector;
	}
//
//	OK, now open the hardware
	r			= this -> rtlsdr_open (&device, deviceIndex);
	if (r < 0) {
	   fprintf (stderr, "Opening dabstick failed\n");
	   *success = false;
	   return;
	}
	open			= true;
	r			= this -> rtlsdr_set_sample_rate (device,
	                                                          inputRate);
	if (r < 0) {
	   fprintf (stderr, "Setting samplerate failed\n");
	   *success = false;
	   return;
	}

	r			= this -> rtlsdr_get_sample_rate (device);
	fprintf (stderr, "samplerate set to %d\n", r);

	gainsCount = rtlsdr_get_tuner_gains (device, NULL);
	fprintf(stderr, "Supported gain values (%d): ", gainsCount);
	gains		= new int [gainsCount];
	gainsCount = rtlsdr_get_tuner_gains (device, gains);
	externalGain	-> setMaximum (gainsCount);
	for (i = gainsCount; i > 0; i--)
		fprintf(stderr, "%.1f ", gains [i - 1] / 10.0);
	rtlsdr_set_tuner_gain_mode (device, 1);
	rtlsdr_set_tuner_gain (device, gains [gainsCount / 2]);

	_I_Buffer		= new RingBuffer<uint8_t>(1024 * 1024);
	workerHandle		= NULL;
	connect (externalGain, SIGNAL (valueChanged (int)),
	         this, SLOT (setExternalGain (int)));
	connect (f_correction, SIGNAL (valueChanged (int)),
	         this, SLOT (freqCorrection  (int)));
	connect (rateSelector, SIGNAL (activated (const QString &)),
	         this, SLOT (set_rateSelector (const QString &)));
	connect (KhzOffset, SIGNAL (valueChanged (int)),
	         this, SLOT (setKhzOffset (int)));
	connect (HzOffset, SIGNAL (valueChanged (int)),
	         this, SLOT (setHzOffset (int)));
	dabSettings	-> beginGroup ("dabstick");
	externalGain -> setValue (dabSettings -> value ("externalGain", 10). toInt ());
	f_correction -> setValue (dabSettings -> value ("f_correction", 0). toInt ());
	KhzOffset	-> setValue (dabSettings -> value ("KhzOffset", 0). toInt ());
	HzOffset	-> setValue (dabSettings -> value ("HzOffset", 0). toInt ());
	dabSettings	-> endGroup ();
	*success 		= true;
	return;

err:
	if (open)
	   this -> rtlsdr_close (device);
#ifdef __MINGW32__
	FreeLibrary (Handle);
#else
	dlclose (Handle);
#endif
	libraryLoaded	= false;
	open		= false;
	*success	= false;
	return;
}

	dabStick::~dabStick	(void) {
	if (open)
	   this -> rtlsdr_close (device);
	if (_I_Buffer != NULL)
	   delete _I_Buffer;
	if (gains != NULL)
	   delete[] gains;

	dabSettings	-> beginGroup ("dabstick");
	dabSettings	-> setValue ("externalGain", externalGain -> value ());
	dabSettings	-> setValue ("f_correction", f_correction -> value ());
	dabSettings	-> setValue ("KhzOffset", KhzOffset -> value ());
	dabSettings	-> setValue ("HzOffset", HzOffset -> value ());
	dabSettings	-> setValue ("dabRate",
	                              rateSelector -> currentText (). toLatin1 (). data ());
	dabSettings	-> endGroup ();
	delete myFrame;
	open = false;
}

void	dabStick::setVFOFrequency	(int32_t f) {
	(void)(this -> rtlsdr_set_center_freq (device, f + vfoOffset));
}

int32_t	dabStick::getVFOFrequency	(void) {
	return (int32_t)(this -> rtlsdr_get_center_freq (device)) - vfoOffset;
}

bool	dabStick::legalFrequency (int32_t f) {
	return  Mhz (24) <= f && f <= Mhz (1400);
}

int32_t	dabStick::defaultFrequency	(void) {
	return Khz (94700);
}
//
//
bool	dabStick::restartReader	(void) {
int32_t	r;

	if (workerHandle != NULL)
	   return true;

	_I_Buffer	-> FlushRingBuffer ();
	r = this -> rtlsdr_reset_buffer (device);
	if (r < 0)
	   return false;

	this -> rtlsdr_set_center_freq (device, 
	                   (int32_t)(this -> rtlsdr_get_center_freq (device)) +
	                                 vfoOffset);
	workerHandle	= new dll_driver (this);
	return true;
}

void	dabStick::stopReader		(void) {
	if (workerHandle == NULL)
	   return;

	this -> rtlsdr_cancel_async (device);
	if (workerHandle != NULL) {
	   while (!workerHandle -> isFinished ()) 
	      usleep (100);

	   delete	workerHandle;
	}

	workerHandle	= NULL;
}
//
//	Note that this function is neither used for the
//	dabreceiver nor for the fmreceiver
//
int32_t	dabStick::setExternalRate	(int32_t newRate) {
int32_t	r;

	if (newRate < 900000) return inputRate;
	if (newRate > 3200000) return inputRate;

	if (workerHandle == NULL) {
	   r	= this -> rtlsdr_set_sample_rate (device, newRate);
	   if (r < 0) {
	      this -> rtlsdr_set_sample_rate (device, inputRate);
	   }

	   r	= this -> rtlsdr_get_sample_rate (device);
	}
	else {	
//	we stop the transmission first
	   stopReader ();
	   r	= this -> rtlsdr_set_sample_rate (device, newRate);
	   if (r < 0) {
	      this -> rtlsdr_set_sample_rate (device, inputRate);
	   }
	   r	= this -> rtlsdr_get_sample_rate (device);
	fprintf (stderr, "samplerate = %d\n", r);
//	ok all set, continue
//	   restartReader (); we need a proper restart from the user
	}

	inputRate	= newRate;
	return r;
}

void	dabStick::setExternalGain	(int gain) {
static int	oldGain	= 0;

	if (gain == oldGain)
	   return;
	if ((gain < 0) || (gain >= gainsCount))
	   return;

	oldGain	= gain;
	rtlsdr_set_tuner_gain (device, gains [gainsCount - gain]);
//	return rtlsdr_get_tuner_gain (device);
	return;
}
//
//	correction is in Hz
void	dabStick::freqCorrection	(int32_t ppm) {
	this -> rtlsdr_set_freq_correction (device, ppm);
}

//
//	The brave old getSamples. For the dab stick, we get
//	size: still in I/Q pairs, but we have to convert the data from
//	uint8_t to DSPCOMPLEX *
int32_t	dabStick::getSamples (DSPCOMPLEX *V, int32_t size) { 
int32_t	amount, i;
uint8_t	*tempBuffer = (uint8_t *)alloca (2 * size * sizeof (uint8_t));
//
	amount = _I_Buffer	-> getDataFromBuffer (tempBuffer, 2 * size);
	for (i = 0; i < amount / 2; i ++)
	    V [i] = DSPCOMPLEX ((float (tempBuffer [2 * i] - 127)) / 128.0,
	                        (float (tempBuffer [2 * i + 1] - 127)) / 128.0);
	return amount / 2;
}

int32_t	dabStick::getSamples 	(DSPCOMPLEX  *V,
	                         int32_t size, uint8_t M) {
	(void)M;
	return getSamples (V, size);
}

int32_t	dabStick::Samples	(void) {
	return _I_Buffer	-> GetRingBufferReadAvailable () / 2;
}
//
uint8_t	dabStick::myIdentity		(void) {
	return DAB_STICK;
}

//	vfoOffset is in Hz, we have two spinboxes influencing the
//	settings
void	dabStick::setKhzOffset	(int k) {
	vfoOffset	= vfoOffset % Khz (1) + Khz (k);
}

void	dabStick::setHzOffset	(int h) {
	vfoOffset	= KHz (1) * vfoOffset / KHz (1) + h;
}

bool	dabStick::load_rtlFunctions (void) {
//
//	link the required procedures
	rtlsdr_open	= (pfnrtlsdr_open)
	                       GETPROCADDRESS (Handle, "rtlsdr_open");
	if (rtlsdr_open == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_open\n");
	   return false;
	}
	rtlsdr_close	= (pfnrtlsdr_close)
	                     GETPROCADDRESS (Handle, "rtlsdr_close");
	if (rtlsdr_close == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_close\n");
	   return false;
	}

	rtlsdr_set_sample_rate =
	    (pfnrtlsdr_set_sample_rate)GETPROCADDRESS (Handle, "rtlsdr_set_sample_rate");
	if (rtlsdr_set_sample_rate == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_set_sample_rate\n");
	   return false;
	}

	rtlsdr_get_sample_rate	=
	    (pfnrtlsdr_get_sample_rate)GETPROCADDRESS (Handle, "rtlsdr_get_sample_rate");
	if (rtlsdr_get_sample_rate == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_sample_rate\n");
	   return false;
	}

	rtlsdr_get_tuner_gains		= (pfnrtlsdr_get_tuner_gains)
	                     GETPROCADDRESS (Handle, "rtlsdr_get_tuner_gains");
	if (rtlsdr_get_tuner_gains == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_tuner_gains\n");
	   return false;
	}


	rtlsdr_set_tuner_gain_mode	= (pfnrtlsdr_set_tuner_gain_mode)
	                     GETPROCADDRESS (Handle, "rtlsdr_set_tuner_gain_mode");
	if (rtlsdr_set_tuner_gain_mode == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_set_tuner_gain_mode\n");
	   return false;
	}

	rtlsdr_set_tuner_gain	= (pfnrtlsdr_set_tuner_gain)
	                     GETPROCADDRESS (Handle, "rtlsdr_set_tuner_gain");
	if (rtlsdr_set_tuner_gain == NULL) {
	   fprintf (stderr, "Cound not find rtlsdr_set_tuner_gain\n");
	   return false;
	}

	rtlsdr_get_tuner_gain	= (pfnrtlsdr_get_tuner_gain)
	                     GETPROCADDRESS (Handle, "rtlsdr_get_tuner_gain");
	if (rtlsdr_get_tuner_gain == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_tuner_gain\n");
	   return false;
	}
	rtlsdr_set_center_freq	= (pfnrtlsdr_set_center_freq)
	                     GETPROCADDRESS (Handle, "rtlsdr_set_center_freq");
	if (rtlsdr_set_center_freq == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_set_center_freq\n");
	   return false;
	}

	rtlsdr_get_center_freq	= (pfnrtlsdr_get_center_freq)
	                     GETPROCADDRESS (Handle, "rtlsdr_get_center_freq");
	if (rtlsdr_get_center_freq == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_center_freq\n");
	   return false;
	}

	rtlsdr_reset_buffer	= (pfnrtlsdr_reset_buffer)
	                     GETPROCADDRESS (Handle, "rtlsdr_reset_buffer");
	if (rtlsdr_reset_buffer == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_reset_buffer\n");
	   return false;
	}

	rtlsdr_read_async	= (pfnrtlsdr_read_async)
	                     GETPROCADDRESS (Handle, "rtlsdr_read_async");
	if (rtlsdr_read_async == NULL) {
	   fprintf (stderr, "Cound not find rtlsdr_read_async\n");
	   return false;
	}

	rtlsdr_get_device_count	= (pfnrtlsdr_get_device_count)
	                     GETPROCADDRESS (Handle, "rtlsdr_get_device_count");
	if (rtlsdr_get_device_count == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_device_count\n");
	   return false;
	}

	rtlsdr_cancel_async	= (pfnrtlsdr_cancel_async)
	                     GETPROCADDRESS (Handle, "rtlsdr_cancel_async");
	if (rtlsdr_cancel_async == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_cancel_async\n");
	   return false;
	}

	rtlsdr_set_direct_sampling = (pfnrtlsdr_set_direct_sampling)
	                  GETPROCADDRESS (Handle, "rtlsdr_set_direct_sampling");
	if (rtlsdr_set_direct_sampling == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_set_direct_sampling\n");
	   return false;
	}

	rtlsdr_set_freq_correction = (pfnrtlsdr_set_freq_correction)
	                  GETPROCADDRESS (Handle, "rtlsdr_set_freq_correction");
	if (rtlsdr_set_freq_correction == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_set_freq_correction\n");
	   return false;
	}
	
	rtlsdr_get_device_name = (pfnrtlsdr_get_device_name)
	                  GETPROCADDRESS (Handle, "rtlsdr_get_device_name");
	if (rtlsdr_get_device_name == NULL) {
	   fprintf (stderr, "Could not find rtlsdr_get_device_name\n");
	   return false;
	}

	fprintf (stderr, "OK, functions seem to be loaded\n");
	return true;
}

void	dabStick::resetBuffer (void) {
	_I_Buffer -> FlushRingBuffer ();
}

int16_t	dabStick::bitDepth	(void) {
	return 8;
}

int32_t	dabStick::getRate	(void) {
	return inputRate;
}

void	dabStick::set_rateSelector (const QString &s) {
int32_t v	= s. toInt ();

	setExternalRate (Khz (v));
	set_changeRate (Khz (v));
}


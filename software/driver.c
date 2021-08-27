/*
	Developed by Daniel Pelikan 2013,2014
	http://digibird1.wordpress.com/
	Reviewed by @kelu124

	Module loaded before reading /dev/hsdk

	Please also refer to this article to have a more verbose version of the module
	https://digibird1.wordpress.com/raspberry-pi-as-an-oscilloscope-10-msps/

	Also building from "Linux Character Device Example "
	https://gist.github.com/brenns10/65d1ee6bb8419f96d2ae693eb7a66cc0
	
	This module is used on a RPi W, using the image from
	http://kghosh.me/img/sdc.img.gz on a 8Gb sd card.
	It compiles just fine.

*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h> // changed from asm/uaccess.h to linux/uaccess.h
#include <linux/time.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/fcntl.h> /*Helps fix O_ACCMODE*/
#include <linux/sched.h> /*Helps fix TASK_UNINTERRUPTIBLE */
#include <linux/fs.h> /*Helps fix the struct intializer */

int __init init_module(void);
void __exit cleanup_module(void);
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

#define SUCCESS 0
#define DEVICE_NAME "hsdk"// Dev name 
#define BUF_LEN 80//Max length of device message 

//---------------------------------------------------------------------------------------------------------
//Things for the GPIO Port 

// depends on the RPi

#define BCM2708_PERI_BASE       0x3F000000 // value needs to be changed to 0x3F000000 for a RPi3. 0x20000000 works for Pi W.
#define GPIO_BASE               (BCM2708_PERI_BASE + 0x200000)	// GPIO controller 

// Defines  GPIO macros to control GPIOs.
#define INP_GPIO(g)   *(gpio.addr + ((g)/10)) &= ~(7<<(((g)%10)*3)) 
#define OUT_GPIO(g)   *(gpio.addr + ((g)/10)) |=  (1<<(((g)%10)*3)) //001
#define SET_GPIO_ALT(g,a) *(gpio.addr + (((g)/10))) |= (((a)<=3?(a) + 4:(a)==4?3:2)<<(((g)%10)*3))
#define GPIO_SET  *(gpio.addr + 7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR  *(gpio.addr + 10) // clears bits which are 1 ignores bits which are 0
#define GPIO_READ(g)  *(gpio.addr + 13) &= (1<<(g))

//GPIO Clock. AT
#define CLOCK_BASE              (BCM2708_PERI_BASE + 0x00101000)
#define GZ_CLK_BUSY (1 << 7)

//---------------------------------------------------------------------------------------------------------

//How many samples to capture
#define SAMPLE_SIZE 	2500 // 2x2500 pts in one line	
#define REPEAT_SIZE 	10 // 10 captures

//static int SAMPLE_SIZE = 2500;
//static int REPEAT_SIZE = 10;

//module_param(SAMPLE_SIZE, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
//module_param(REPEAT_SIZE, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

// in this setup, there will be REPEAT_SIZE captures, of each SAMPLE_SIZE length, with ADCs interleaved, 
// that means 2x SAMPLE_SIZE length

//Define GPIO Pins

//ADC 1
#define BIT0_ADC1 16
#define BIT1_ADC1 17
#define BIT2_ADC1 18
#define BIT3_ADC1 19
#define BIT4_ADC1 20
#define BIT5_ADC1 22
#define BIT6_ADC1 25
#define BIT7_ADC1 26
#define BIT8_ADC1 27

//ADC 2
#define BIT0_ADC2 7
#define BIT1_ADC2 8
#define BIT2_ADC2 9
#define BIT3_ADC2 10
#define BIT4_ADC2 11
#define BIT5_ADC2 12
#define BIT6_ADC2 13
#define BIT7_ADC2 14
#define BIT8_ADC2 15

// Pulser
#define Puls_ON  23
#define Puls_OFF 24

#define PPWWMM 6

#define MY_NOP(__N)                 __asm ("nop");    // or sth like "MOV R0,R0"


//---------------------------------------------------------------------------------------------------------

// IO Acces
struct bcm2835_peripheral {
	unsigned long addr_p;
	int mem_fd;
	void *map;
	volatile unsigned int *addr;

};

static int map_peripheral(struct bcm2835_peripheral *p);
static void unmap_peripheral(struct bcm2835_peripheral *p);
static void readScope(void);

static int Major;		/* Major number assigned to our device driver */
static int Device_Open = 0;	/* Is device open?  
				 * Used to prevent multiple access to device */
static char msg[BUF_LEN];	/* The msg the device will give when asked */
static char *msg_Ptr;

// unused variable
static uint32_t *ScopeBuffer_Ptr;
// changed from unsigned char
static uint32_t *buf_p;

//changed
static struct file_operations fops = {
	read : device_read,
	write : device_write,
	open : device_open,
	release : device_release
};
// old
// both should work 
/*
static struct file_operations fops = {
		.read = device_read,
		.write = device_write,
		.open = device_open,
		.release = device_release
}
*/
//---------------------------------------------------------------------------------------------------------

/*
We need to assign the addresses of GPIO and the clock to a variable that we can find the hardware. A data structure is defined to hold our values we read out from the ADC, as well as the time from start of the readout to the end of the readout. This time is needed in order to calculate the time step between each sample. Additional two pointers are defined for later operations.
*/

static struct bcm2835_peripheral myclock = {CLOCK_BASE};

static struct bcm2835_peripheral gpio = {GPIO_BASE};


typedef struct DataStruct{
	uint32_t Buffer[REPEAT_SIZE*SAMPLE_SIZE];
	uint32_t time;
}Ds; 
// adding identifier above and using it 
// static makes it invisible outside the file, I think this 
// does not affect the module
Ds dataStruct;

// changed from unsigned char
static uint32_t *ScopeBufferStart;
static uint32_t *ScopeBufferStop;

//---------------------------------------------------------------------------------------------------------
/*
Since we want to manipulate hardware registers we need to map the hardware registers into memory. This can be done by two functions, one for the mapping and one for the unmapping.
*/
static int map_peripheral(struct bcm2835_peripheral *p)
{
	p->addr=(uint32_t *)ioremap(GPIO_BASE, 41*4); //41 GPIO register with 32 bit (4*8)
   return 0;
}
 
static void unmap_peripheral(struct bcm2835_peripheral *p) {
 	iounmap(p->addr);//unmap the address
}


//---------------------------------------------------------------------------------------------------------
/*
 In our case we are only taking 10k samples so not too much time. Before the sample taking we take a time stamp. Then we read out 10k times the GPIO register and save it in our data structure. The GPIO register is a 32bit value so it is made out of 32 ‘1’s and ‘0’s each defining if the GPIO port is high (3.3V) or low (GND). After the read out we take another time stamp and turn on all interrupts again. The two time stamps we took are important since we can calculate how long it took to read in the 10k samples. The time difference divided by 10k gives us the time between each sample point. In case the sample frequency is too high and should be reduced one can add some delay and waste some time during each readout step. Here the aim is to achieve the maximal performance.
*/

static void readScope(){

	int counter=0;
	int counterline = 0;
	int limit = 0;

	int Pon=0; 
	int Poff=0;
	//int Fail=0;

	// moved from line 203
	// fix for the compiler warning of mixed declarations
	struct timespec64 ts_start,ts_stop;

	// Setting GPIOs
	OUT_GPIO(Puls_ON); 
	OUT_GPIO(Puls_OFF);

	GPIO_SET = 1 << Puls_ON; 
	GPIO_CLR = 1 << Puls_OFF;

	msleep(10);

	//disable IRQ
	local_irq_disable();
	local_fiq_disable();


	//Start time

	set_current_state(TASK_UNINTERRUPTIBLE);
	// Fixed with this: https://github.com/torvalds/linux/blob/e9a83bd2322035ed9d7dcf35753d3f984d76c6a5/Documentation/core-api/timekeeping.rst
	ktime_get_real_ts64(&ts_start);

	while(counterline<REPEAT_SIZE){ 
		Pon = 0; 
		Poff = 0;
		limit = (counterline+1)*SAMPLE_SIZE;

		//printk(KERN_INFO "Shooting line %d\n", counterline);


		// 	NOP Calibration in standard use	
		//	10 NOPs: 200ns
		//	20 NOPs: 250ns
		//	150 NOPs: 750ns
		//	1500 NOPS: 7500ns


		//printk(KERN_ALERT "Starting line acquisition number %d\n", counterline);
		GPIO_SET = 1 << Puls_ON;
		while(Pon<10){ // Nb of NOPs for PulseOn 
			MY_NOP(__N); // 200ns
			Pon++;
		}
		GPIO_CLR = 1 << Puls_ON;


		GPIO_CLR = 1 << Puls_OFF;
		while(Poff<3500){ // Nb of NOPs for PulseOff. 
			MY_NOP(__N); // a few us off
			Poff++;
		}
		GPIO_SET = 1 << Puls_OFF;


		while(counter<(limit) ){
			dataStruct.Buffer[counter++]= *(gpio.addr + 13); 
		}
	
		// to avoid freezes
		msleep(0.5);

		counterline++;
	}



	//Stop time
	ktime_get_real_ts64(&ts_stop);

	INP_GPIO(Puls_ON); 
	INP_GPIO(Puls_OFF);

	set_current_state(TASK_INTERRUPTIBLE);
	//enable IRQ
	local_fiq_enable();
	local_irq_enable();

	//save the time difference
	dataStruct.time=timespec64_to_ns(&ts_stop)-timespec64_to_ns(&ts_start);//ns resolution
	buf_p= dataStruct.Buffer;//cound maybe removed

	//accessing memeber of the structure that is already pointer by its nature
	ScopeBufferStart= dataStruct.Buffer;
	ScopeBufferStop=ScopeBufferStart+sizeof(struct DataStruct);
}

//---------------------------------------------------------------------------------------------------------
/*
In order to make a kernel module work the module needs some special entry functions. One of these functions is the init_module(void) which is called when the kernel module is loaded. Here the function to map the periphery is called, the GPIO pins are defined as inputs and a device file in /dev/ is created for communication with the kernel module. Additionally a 10 MHz clock signal on the GPIO Pin 4 is defined. This clock signal is needed in order to feed the ADC with an input clock. A 500 MHz signal from a PLL is used and the clock divider is set to divide by 50, which gives the required 10 MHz signal. More details on this clock can found in chapter 6.3 General Purpose GPIO Clocks in [4]. 
*/
/*
 * This function is called when the module is loaded
 */
int init_module(void)
{

	// moved from line 339 
	// fix for the compiler warning of mixed declarations
	struct bcm2835_peripheral *p=&myclock;
	// moved from line 348 
	// fix for the compiler warning of mixed declarations
	int speed_id = 6; //1 for to start with 19Mhz or 6 to start with 500 MHz

    Major = register_chrdev(0, DEVICE_NAME, &fops);

	if (Major < 0) {
	  printk(KERN_ALERT "Registering char device failed with %d\n", Major);
	  return Major;
	}

	printk(KERN_INFO "I was assigned major number %d. To talk to\n", Major);
	printk(KERN_INFO "the driver, create a dev file with\n");
	printk(KERN_INFO "'mknod /dev/%s c %d 0'.\n", DEVICE_NAME, Major);
	printk(KERN_INFO "Try various minor numbers. Try to cat and echo to\n");
	printk(KERN_INFO "the device file.\n");
	printk(KERN_INFO "Remove the device file and module when done.\n");

	//Map GPIO

	if(map_peripheral(&gpio) == -1) 
	{
		// goes without comma
		printk(KERN_ALERT "Failed to map the physical GPIO registers into the virtual memory space.\n");
		return -1;
	}

	//Define Scope pins as inputs
	// ADC1 
	INP_GPIO(BIT0_ADC1);
	INP_GPIO(BIT1_ADC1);
	INP_GPIO(BIT2_ADC1);
	INP_GPIO(BIT3_ADC1);
	INP_GPIO(BIT4_ADC1);
	INP_GPIO(BIT5_ADC1);
	INP_GPIO(BIT6_ADC1);
	INP_GPIO(BIT7_ADC1);
	INP_GPIO(BIT8_ADC1);
	// ADC2
	INP_GPIO(BIT0_ADC2);
	INP_GPIO(BIT1_ADC2);
	INP_GPIO(BIT2_ADC2);
	INP_GPIO(BIT3_ADC2);
	INP_GPIO(BIT4_ADC2);
	INP_GPIO(BIT5_ADC2);
	INP_GPIO(BIT6_ADC2);
	INP_GPIO(BIT7_ADC2);
	INP_GPIO(BIT8_ADC2);

	// Setting pins for pulser
	// This section only useful for use on ultrasound hardware
	OUT_GPIO(Puls_ON); 
	OUT_GPIO(Puls_OFF);
	GPIO_CLR = 1 << Puls_ON; // set pulser at 0
	GPIO_SET = 1 << Puls_OFF; // set damper at 1

	//Set a clock signal on Pin 4
	p->addr=(uint32_t *)ioremap(CLOCK_BASE, 41*4);
 	INP_GPIO(4);
	SET_GPIO_ALT(4,0);
	// Preparing the clock

	*(myclock.addr+28)=0x5A000000 | speed_id; //Turn off the clock
	while (*(myclock.addr+28) & GZ_CLK_BUSY) {}; //Wait until clock is no longer busy (BUSY flag)
	// Set divider //divide by 50 (0x32) -- ideally 41 (29) to fall on 12MHz clock
	*(myclock.addr+29)= 0x5A000000 | (0x29 << 12) | 0;
	// And let's turn clock on
	*(myclock.addr+28)=0x5A000010 | speed_id;

	return SUCCESS;
}
//---------------------------------------------------------------------------------------------------------
/*
 * This function is called when the module is unloaded
 */
void cleanup_module(void)
{
	unregister_chrdev(Major, DEVICE_NAME);
	unmap_peripheral(&gpio);
	unmap_peripheral(&myclock);
}
//---------------------------------------------------------------------------------------------------------
/* 
 * Called when a process tries to open the device file, like
 * "cat /dev/mycharfile"
 */
/*
Furthermore a function is needed which is called when the device file belonging to our kernel module is opened. When this happens the measurement is done by calling the readScope() function and saved in memory.
*/
static int device_open(struct inode *inode, struct file *file)
{
	static int counter = 0;

	if (Device_Open)
		return -EBUSY;

	Device_Open++;
	sprintf(msg, "I already told you %d times Hello world!\n", counter++);
	msg_Ptr = msg;

	readScope();//Read n Samples into memory

	try_module_get(THIS_MODULE);

	return SUCCESS;
}
//---------------------------------------------------------------------------------------------------------
/* 
 * Called when a process closes the device file.
 */
static int device_release(struct inode *inode, struct file *file)
{
	Device_Open--;		/* We're now ready for our next caller */
	module_put(THIS_MODULE);
	return 0;
}

//---------------------------------------------------------------------------------------------------------
/* 
 * Called when a process, which already opened the dev file, attempts to
 * read from it.
 * When the device is open we can read from it which calls the function device_read() in kernel space. 
 * This returns the measurement we made when we opened the device. 
 * Here one could also add a call of the function readScope() in order to do a permanent readout. 
 * As the code is right now one needs to open the device file for each new measurement, read from it and close it. 
 * But we leave it like this for the sake of simplicity.
 */
static ssize_t device_read(struct file *filp,	
			   char *buffer,	
			   size_t length,
			   loff_t * offset)
{

	// Number of bytes actually written to the buffer 
	int bytes_read = 0;

	if (*msg_Ptr == 0)
		return 0;

	//Check that we do not overfill the buffer

	while (length && buf_p<ScopeBufferStop) {

		if(0!=put_user(*(buf_p++), buffer++))
			printk(KERN_INFO "Problem with copy\n");
		length--;
		bytes_read++;
	}

	return bytes_read;
}
//---------------------------------------------------------------------------------------------------------
/*  
 * Called when a process writes to dev file: echo "hi" > /dev/hello 

The last step to make our kernel module complete is to define a function 
that is called when we want to write into the device file. 
But this functions does nothing except for writing an error message, 
since we do not want write support yet.
However, one could use this to allow a user to control variables
from user space.
 */

static ssize_t
device_write(struct file *filp, const char *buff, size_t len, loff_t * off)
{
	printk(KERN_ALERT "Sorry, this operation isn't supported.\n");
	return -EINVAL;
}


MODULE_AUTHOR("kelu124");
MODULE_LICENSE("GPL"); // HOLY GPL !
MODULE_VERSION("2");


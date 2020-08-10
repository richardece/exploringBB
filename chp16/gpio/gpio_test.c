/**
 * @file   gpio_test.c
 * @author Derek Molloy
 * @date   8 November 2015
 * @brief  A kernel module for controlling a GPIO LED/button pair. The
 * device mounts an LED and pushbutton via sysfs /sys/class/gpio/gpio60
 * and gpio46 respectively. */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>                 // for the GPIO functions
#include <linux/interrupt.h>            // for the IRQ code
#include <linux/kobject.h>              // Using kobjects for the sysfs bindings

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Derek Molloy");
MODULE_DESCRIPTION("A Button/LED test driver for the Beagle");
MODULE_VERSION("0.1");

//used when LKM is loaded to kernel
static char *name = "world"; // example LKM argument default is "world"
// param description charp = char pointer, defaults to "world"
module_param(name, charp, S_IRUGO); // S_IRUGO can be read/not changed
MODULE_PARM_DESC(name, "The name to display in /var/log/kern.log");

static unsigned int gpioButton = 46;    // P8_16/P2.22 (GPIO46)

//sysfs entries to control the LKM
//read-only
static unsigned int irqNumber = 0;      // share IRQ num within file
static unsigned int gpioState = 0;      // share IRQ num within file
//read/write
static unsigned int numberPresses = 0;  // store number of presses


// prototype for the custom IRQ handler function
static irq_handler_t  ebb_gpio_irq_handler(unsigned int irq, void
                                    *dev_id, struct pt_regs *regs);

/** @brief A callback function to output the irqNumber variable
 *  @param kobj a kernel object device that appears in the sysfs filesystem
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer to which to write the number of presses
 *  @return return the total number of characters written to the buffer
 */
static ssize_t irqNumber_show(struct kobject *kobj, struct kobj_attribute *attr, 
                          char *buf) {
   return sprintf(buf, "%d\n", irqNumber);
}

/** @brief Displays the GPIO state */
static ssize_t gpioState_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf) {
   return sprintf(buf, "%d\n", gpioState);
}

/** @brief A callback function to read in (from sysfs) the numberPresses variable */
static ssize_t numberPresses_store(struct kobject *kobj, struct
                    kobj_attribute *attr, const char *buf, size_t count) {
   sscanf(buf, "%du", &numberPresses);
   return count;
}
//WHAT is count??

/** @brief Displays the number of presses */
static ssize_t numberPresses_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf) {
   return sprintf(buf, "%d\n", numberPresses);
}

/**  The __ATTR_RO macro defines a read-only attribute. There is no need to
 * identify that the function is called _show, but it must be present.
 * __ATTR_WO can be  used for a write-only attribute but only Linux 3.11.x+
*/
static struct kobj_attribute irqNumber_attr = __ATTR_RO(irqNumber);
static struct kobj_attribute gpioState_attr = __ATTR_RO(gpioState);

/**  Use these helper macros to define the name and access levels of the
 * kobj_attributes. The kobj_attribute has an attribute attr (name and mode),
 * show and store function pointers. The count variable is associated with
 * the numberPresses variable and it is to be exposed with mode 0664 using
 * the numberPresses_show and numberPresses_store functions above
 */
static struct kobj_attribute count_attr = __ATTR(numberPresses, 0664, numberPresses_show, numberPresses_store);
//WHAT does 0664 mean??

/**  The ebb_attrs[] is an array of attributes that is used to create the
 * attribute group below. The attr property of the kobj_attribute is used
 * to extract the attribute struct
 */
static struct attribute *ebb_attrs[] = {
      &count_attr.attr,        // the number of button presses
      &irqNumber_attr.attr,    // IRQ number
      &gpioState_attr.attr,    // GPIO state
      NULL,
};

/**  The attribute group uses the attribute array and a name, which is
 * exposed on sysfs
 */
static struct attribute_group attr_group = {
      .name  = "gpio46",       // the name that will appear in sysfs
      .attrs = ebb_attrs,      // the attributes array defined just above
};

static struct kobject *ebb_kobj;

/** @brief The LKM initialization function */
static int __init ebb_gpio_init(void){
   int result = 0;
   printk(KERN_INFO "GPIO_TEST: Hello %s from GPIO_TEST LKM!\n", name);

   // create the kobject sysfs entry at /sys/ebb
   // kernel_kobj points to /sys/kernel, while kernel_kobj-> points to /sys
   ebb_kobj = kobject_create_and_add("ebb", kernel_kobj->parent);
   if(!ebb_kobj){
      printk(KERN_ALERT "EBB Button: failed to create kobject mapping\n");
      return -ENOMEM;
   }
   // add the attributes to /sys/ebb/ e.g., /sys/ebb/gpio46/numberPresses
   result = sysfs_create_group(ebb_kobj, &attr_group);
   if(result) {
      printk(KERN_ALERT "EBB Button: failed to create sysfs group\n");
      kobject_put(ebb_kobj);               // clean up remove entry
      return result;
   }

   gpio_request(gpioButton, "sysfs");       // set up gpioButton
   gpio_direction_input(gpioButton);        // set up as input
//   gpio_set_debounce(gpioButton, 200);      // debounce delay of 200ms
   gpio_export(gpioButton, false);          // appears in /sys/class/gpio

   gpioState = gpio_get_value(gpioButton);
   printk(KERN_INFO "GPIO_TEST: button value is currently: %d\n",
         gpioState );
   irqNumber = gpio_to_irq(gpioButton);     // map GPIO to IRQ number
   printk(KERN_INFO "GPIO_TEST: button mapped to IRQ: %d\n", irqNumber);

   // This next call requests an interrupt line
   result = request_irq(irqNumber,         // interrupt number requested
            (irq_handler_t) ebb_gpio_irq_handler, // handler function
            IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING,  // on rising edge (press, not release)
            "ebb_gpio_handler",  // used in /proc/interrupts
            NULL);                // *dev_id for shared interrupt lines
   printk(KERN_INFO "GPIO_TEST: IRQ request result is: %d\n", result);
   return result;
}

/** @brief The LKM cleanup function  */
static void __exit ebb_gpio_exit(void){

   printk(KERN_INFO "GPIO_TEST: pressed %d times\n", numberPresses);

   kobject_put(ebb_kobj);         // clean up, remove kobject sysfs entry

   free_irq(irqNumber, NULL);     // free the IRQ number, no *dev_id
   gpio_unexport(gpioButton);     // unexport the Button GPIO
   gpio_free(gpioButton);         // free the Button GPIO

   printk(KERN_INFO "GPIO_TEST: Goodbye %s from GPIO_TEST LKM!\n", name);

}

/** @brief The GPIO IRQ Handler function
 * A custom interrupt handler that is attached to the GPIO. The same
 * interrupt handler cannot be invoked concurrently as the line is
 * masked out until the function is complete. This function is static
 * as it should not be invoked directly from outside of this file.
 * @param irq    the IRQ number associated with the GPIO
 * @param dev_id the *dev_id that is provided - used to identify
 * which device caused the interrupt. Not used here.
 * @param regs   h/w specific register values -used for debugging.
 * return returns IRQ_HANDLED if successful - return IRQ_NONE otherwise.
 */
static irq_handler_t ebb_gpio_irq_handler(unsigned int irq,
                        void *dev_id, struct pt_regs *regs)
                        {
   gpioState = gpio_get_value(gpioButton);

   if (0 == gpioState)	//if falling edge
   {
      numberPresses++;                    // global counter
   }

   printk(KERN_INFO "GPIO_TEST: Button is %d. Pressed %d times.\n",
          gpioState, numberPresses);

   return (irq_handler_t) IRQ_HANDLED; // announce IRQ handled
}

module_init(ebb_gpio_init);
module_exit(ebb_gpio_exit);

/*-
 * Copyright (c) 2013 Ganbold Tsagaankhuu <ganbold@freebsd.org>
 * Copyright (c) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * Copyright (c) 2012 Luiz Otavio O Souza.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/gpio.h>

#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/resource.h>
#include <machine/intr.h>

#include <dev/fdt/fdt_common.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm/allwinner/allwinner_machdep.h>
#include <arm/allwinner/allwinner_pinctrl.h>

#include "gpio_if.h"

/*
 * A10 have 9 banks of gpio.
 * 32 pins per bank:
 * PA0 - PA17 | PB0 - PB23 | PC0 - PC24
 * PD0 - PD27 | PE0 - PE31 | PF0 - PF5
 * PG0 - PG9 | PH0 - PH27 | PI0 - PI12
 */

#define	A10_GPIO_DEFAULT_CAPS	(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT |	\
    GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)

#define A10_GPIO_NONE		0
#define A10_GPIO_PULLUP		1
#define A10_GPIO_PULLDOWN	2

#define A10_GPIO_INPUT		0
#define A10_GPIO_OUTPUT		1

#define AW_GPIO_DRV_MASK	0x3
#define AW_GPIO_PUD_MASK	0x3

static struct ofw_compat_data compat_data[] = {
	{"allwinner,sun4i-a10-pinctrl", 1},
	{"allwinner,sun7i-a20-pinctrl", 1},
	{NULL,             0}
};

struct a10_gpio_softc {
	device_t		sc_dev;
	device_t		sc_busdev;
	struct mtx		sc_mtx;
	struct resource *	sc_mem_res;
	struct resource *	sc_irq_res;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	void *			sc_intrhand;
	const struct allwinner_padconf *	padconf;
};

/* Defined in a10_padconf.c */
#ifdef SOC_ALLWINNER_A10
extern const struct allwinner_padconf a10_padconf;
#endif

/* Defined in a20_padconf.c */
#ifdef SOC_ALLWINNER_A20
extern const struct allwinner_padconf a20_padconf;
#endif

#define	A10_GPIO_LOCK(_sc)		mtx_lock_spin(&(_sc)->sc_mtx)
#define	A10_GPIO_UNLOCK(_sc)		mtx_unlock_spin(&(_sc)->sc_mtx)
#define	A10_GPIO_LOCK_ASSERT(_sc)	mtx_assert(&(_sc)->sc_mtx, MA_OWNED)

#define	A10_GPIO_GP_CFG(_bank, _idx)	0x00 + ((_bank) * 0x24) + ((_idx) << 2)
#define	A10_GPIO_GP_DAT(_bank)		0x10 + ((_bank) * 0x24)
#define	A10_GPIO_GP_DRV(_bank, _idx)	0x14 + ((_bank) * 0x24) + ((_idx) << 2)
#define	A10_GPIO_GP_PUL(_bank, _idx)	0x1c + ((_bank) * 0x24) + ((_idx) << 2)

#define	A10_GPIO_GP_INT_CFG0		0x200
#define	A10_GPIO_GP_INT_CFG1		0x204
#define	A10_GPIO_GP_INT_CFG2		0x208
#define	A10_GPIO_GP_INT_CFG3		0x20c

#define	A10_GPIO_GP_INT_CTL		0x210
#define	A10_GPIO_GP_INT_STA		0x214
#define	A10_GPIO_GP_INT_DEB		0x218

static struct a10_gpio_softc *a10_gpio_sc;

#define	A10_GPIO_WRITE(_sc, _off, _val)		\
    bus_space_write_4(_sc->sc_bst, _sc->sc_bsh, _off, _val)
#define	A10_GPIO_READ(_sc, _off)		\
    bus_space_read_4(_sc->sc_bst, _sc->sc_bsh, _off)

static uint32_t
a10_gpio_get_function(struct a10_gpio_softc *sc, uint32_t pin)
{
	uint32_t bank, func, offset;

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	if (pin > sc->padconf->npins)
		return (0);
	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;
	offset = ((pin & 0x07) << 2);

	func = A10_GPIO_READ(sc, A10_GPIO_GP_CFG(bank, pin >> 3));
	switch ((func >> offset) & 0x7) {
	case A10_GPIO_INPUT:
		return (GPIO_PIN_INPUT);
	case A10_GPIO_OUTPUT:
		return (GPIO_PIN_OUTPUT);
	}

	return (0);
}

static void
a10_gpio_set_function(struct a10_gpio_softc *sc, uint32_t pin, uint32_t f)
{
	uint32_t bank, data, offset;

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;
	offset = ((pin & 0x07) << 2);

	data = A10_GPIO_READ(sc, A10_GPIO_GP_CFG(bank, pin >> 3));
	data &= ~(7 << offset);
	data |= (f << offset);
	A10_GPIO_WRITE(sc, A10_GPIO_GP_CFG(bank, pin >> 3), data);
}

static uint32_t
a10_gpio_get_pud(struct a10_gpio_softc *sc, uint32_t pin)
{
	uint32_t bank, offset, val;

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;
	offset = ((pin & 0x0f) << 1);

	val = A10_GPIO_READ(sc, A10_GPIO_GP_PUL(bank, pin >> 4));
	switch ((val >> offset) & 0x3) {
	case A10_GPIO_PULLDOWN:
		return (GPIO_PIN_PULLDOWN);
	case A10_GPIO_PULLUP:
		return (GPIO_PIN_PULLUP);
	}

	return (0);
}

static void
a10_gpio_set_pud(struct a10_gpio_softc *sc, uint32_t pin, uint32_t state)
{
	uint32_t bank, offset, val;

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;
	offset = ((pin & 0x0f) << 1);

	val = A10_GPIO_READ(sc, A10_GPIO_GP_PUL(bank, pin >> 4));
	val &= ~(AW_GPIO_PUD_MASK << offset);
	val |= (state << offset);
	A10_GPIO_WRITE(sc, A10_GPIO_GP_PUL(bank, pin >> 4), val);
}

static void
a10_gpio_set_drv(struct a10_gpio_softc *sc, uint32_t pin, uint32_t drive)
{
	uint32_t bank, offset, val;

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;
	offset = ((pin & 0x0f) << 1);

	val = A10_GPIO_READ(sc, A10_GPIO_GP_DRV(bank, pin >> 4));
	val &= ~(AW_GPIO_DRV_MASK << offset);
	val |= (drive << offset);
	A10_GPIO_WRITE(sc, A10_GPIO_GP_DRV(bank, pin >> 4), val);
}

static void
a10_gpio_pin_configure(struct a10_gpio_softc *sc, uint32_t pin, uint32_t flags)
{

	/* Must be called with lock held. */
	A10_GPIO_LOCK_ASSERT(sc);

	/* Manage input/output. */
	if (flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)) {
		if (flags & GPIO_PIN_OUTPUT)
			a10_gpio_set_function(sc, pin, A10_GPIO_OUTPUT);
		else
			a10_gpio_set_function(sc, pin, A10_GPIO_INPUT);
	}

	/* Manage Pull-up/pull-down. */
	if (flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)) {
		if (flags & GPIO_PIN_PULLUP)
			a10_gpio_set_pud(sc, pin, A10_GPIO_PULLUP);
		else
			a10_gpio_set_pud(sc, pin, A10_GPIO_PULLDOWN);
	} else
		a10_gpio_set_pud(sc, pin, A10_GPIO_NONE);
}

static device_t
a10_gpio_get_bus(device_t dev)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);

	return (sc->sc_busdev);
}

static int
a10_gpio_pin_max(device_t dev, int *maxpin)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);

	*maxpin = sc->padconf->npins - 1;
	return (0);
}

static int
a10_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (pin >= sc->padconf->npins)
		return (EINVAL);

	*caps = A10_GPIO_DEFAULT_CAPS;

	return (0);
}

static int
a10_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (pin >= sc->padconf->npins)
		return (EINVAL);

	A10_GPIO_LOCK(sc);
	*flags = a10_gpio_get_function(sc, pin);
	*flags |= a10_gpio_get_pud(sc, pin);
	A10_GPIO_UNLOCK(sc);

	return (0);
}

static int
a10_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (pin >= sc->padconf->npins)
		return (EINVAL);

	snprintf(name, GPIOMAXNAME - 1, "%s",
	    sc->padconf->pins[pin].name);
	name[GPIOMAXNAME - 1] = '\0';

	return (0);
}

static int
a10_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (pin > sc->padconf->npins)
		return (EINVAL);

	A10_GPIO_LOCK(sc);
	a10_gpio_pin_configure(sc, pin, flags);
	A10_GPIO_UNLOCK(sc);

	return (0);
}

static int
a10_gpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct a10_gpio_softc *sc;
	uint32_t bank, data;

	sc = device_get_softc(dev);
	if (pin > sc->padconf->npins)
		return (EINVAL);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;

	A10_GPIO_LOCK(sc);
	data = A10_GPIO_READ(sc, A10_GPIO_GP_DAT(bank));
	if (value)
		data |= (1 << pin);
	else
		data &= ~(1 << pin);
	A10_GPIO_WRITE(sc, A10_GPIO_GP_DAT(bank), data);
	A10_GPIO_UNLOCK(sc);

	return (0);
}

static int
a10_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *val)
{
	struct a10_gpio_softc *sc;
	uint32_t bank, reg_data;

	sc = device_get_softc(dev);
	if (pin > sc->padconf->npins)
		return (EINVAL);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;

	A10_GPIO_LOCK(sc);
	reg_data = A10_GPIO_READ(sc, A10_GPIO_GP_DAT(bank));
	A10_GPIO_UNLOCK(sc);
	*val = (reg_data & (1 << pin)) ? 1 : 0;

	return (0);
}

static int
a10_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct a10_gpio_softc *sc;
	uint32_t bank, data;

	sc = device_get_softc(dev);
	if (pin > sc->padconf->npins)
		return (EINVAL);

	bank = sc->padconf->pins[pin].port;
	pin = sc->padconf->pins[pin].pin;

	A10_GPIO_LOCK(sc);
	data = A10_GPIO_READ(sc, A10_GPIO_GP_DAT(bank));
	if (data & (1 << pin))
		data &= ~(1 << pin);
	else
		data |= (1 << pin);
	A10_GPIO_WRITE(sc, A10_GPIO_GP_DAT(bank), data);
	A10_GPIO_UNLOCK(sc);

	return (0);
}

static int
aw_find_pinnum_by_name(struct a10_gpio_softc *sc, const char *pinname)
{
	int i;

	for (i = 0; i < sc->padconf->npins; i++)
		if (!strcmp(pinname, sc->padconf->pins[i].name))
			return i;

	return (-1);
}

static int
aw_find_pin_func(struct a10_gpio_softc *sc, int pin, const char *func)
{
	int i;

	for (i = 0; i < AW_MAX_FUNC_BY_PIN; i++)
		if (sc->padconf->pins[pin].functions[i] &&
		    !strcmp(func, sc->padconf->pins[pin].functions[i]))
			return (i);

	return (-1);
}

static int
aw_fdt_configure_pins(device_t dev, phandle_t cfgxref)
{
	struct a10_gpio_softc *sc;
	phandle_t node;
	const char **pinlist = NULL;
	char *pin_function = NULL;
	uint32_t pin_drive, pin_pull;
	int pins_nb, pin_num, pin_func, i, ret;

	sc = device_get_softc(dev);
	node = OF_node_from_xref(cfgxref);
	ret = 0;

	/* Getting all prop for configuring pins */
	pins_nb = ofw_bus_string_list_to_array(node, "allwinner,pins", &pinlist);
	if (pins_nb <= 0)
		return (ENOENT);
	if (OF_getprop_alloc(node, "allwinner,function",
			     sizeof(*pin_function),
			     (void **)&pin_function) == -1) {
		ret = ENOENT;
		goto out;
	}
	if (OF_getencprop(node, "allwinner,drive",
			  &pin_drive, sizeof(pin_drive)) == -1) {
		ret = ENOENT;
		goto out;
	}
	if (OF_getencprop(node, "allwinner,pull",
			  &pin_pull, sizeof(pin_pull)) == -1) {
		ret = ENOENT;
		goto out;
	}

	/* Configure each pin to the correct function, drive and pull */
	for (i = 0; i < pins_nb; i++) {
		pin_num = aw_find_pinnum_by_name(sc, pinlist[i]);
		if (pin_num == -1) {
			ret = ENOENT;
			goto out;
		}
		pin_func = aw_find_pin_func(sc, pin_num, pin_function);
		if (pin_func == -1) {
			ret = ENOENT;
			goto out;
		}

		A10_GPIO_LOCK(sc);
		a10_gpio_set_function(sc, pin_num, pin_func);
		a10_gpio_set_drv(sc, pin_num, pin_drive);
		a10_gpio_set_pud(sc, pin_num, pin_pull);
		A10_GPIO_UNLOCK(sc);
	}

 out:
	free(pinlist, M_OFWPROP);
	free(pin_function, M_OFWPROP);
	return (ret);
}

static int
a10_gpio_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Allwinner GPIO/Pinmux controller");
	return (BUS_PROBE_DEFAULT);
}

static int
a10_gpio_attach(device_t dev)
{
	int rid;
	phandle_t gpio;
	struct a10_gpio_softc *sc;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;

	mtx_init(&sc->sc_mtx, "a10 gpio", "gpio", MTX_SPIN);

	rid = 0;
	sc->sc_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (!sc->sc_mem_res) {
		device_printf(dev, "cannot allocate memory window\n");
		goto fail;
	}

	sc->sc_bst = rman_get_bustag(sc->sc_mem_res);
	sc->sc_bsh = rman_get_bushandle(sc->sc_mem_res);

	rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	if (!sc->sc_irq_res) {
		device_printf(dev, "cannot allocate interrupt\n");
		goto fail;
	}

	/* Find our node. */
	gpio = ofw_bus_get_node(sc->sc_dev);
	if (!OF_hasprop(gpio, "gpio-controller"))
		/* Node is not a GPIO controller. */
		goto fail;

	a10_gpio_sc = sc;
	sc->sc_busdev = gpiobus_attach_bus(dev);
	if (sc->sc_busdev == NULL)
		goto fail;


	/* Use the right pin data for the current SoC */
	switch (allwinner_soc_type()) {
#ifdef SOC_ALLWINNER_A10
	case ALLWINNERSOC_A10:
		sc->padconf = &a10_padconf;
		break;
#endif
#ifdef SOC_ALLWINNER_A20
	case ALLWINNERSOC_A20:
		sc->padconf = &a20_padconf;
		break;
#endif
	default:
		return (ENOENT);
	}

	/*
	 * Register as a pinctrl device
	 */
	fdt_pinctrl_register(dev, "allwinner,pins");
	fdt_pinctrl_configure_tree(dev);

	return (0);

fail:
	if (sc->sc_irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->sc_irq_res);
	if (sc->sc_mem_res)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->sc_mem_res);
	mtx_destroy(&sc->sc_mtx);

	return (ENXIO);
}

static int
a10_gpio_detach(device_t dev)
{

	return (EBUSY);
}

static phandle_t
a10_gpio_get_node(device_t dev, device_t bus)
{

	/* We only have one child, the GPIO bus, which needs our own node. */
	return (ofw_bus_get_node(dev));
}

static int
a10_gpio_map_gpios(device_t bus, phandle_t dev, phandle_t gparent, int gcells,
    pcell_t *gpios, uint32_t *pin, uint32_t *flags)
{
	struct a10_gpio_softc *sc;
	int i;

	sc = device_get_softc(bus);

	/* The GPIO pins are mapped as: <gpio-phandle bank pin flags>. */
	for (i = 0; i < sc->padconf->npins; i++)
		if (sc->padconf->pins[i].port == gpios[0] &&
		    sc->padconf->pins[i].pin == gpios[1]) {
			*pin = i;
			break;
		}
	*flags = gpios[gcells - 1];

	return (0);
}

static device_method_t a10_gpio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		a10_gpio_probe),
	DEVMETHOD(device_attach,	a10_gpio_attach),
	DEVMETHOD(device_detach,	a10_gpio_detach),

	/* GPIO protocol */
	DEVMETHOD(gpio_get_bus,		a10_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,		a10_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	a10_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags,	a10_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps,	a10_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags,	a10_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		a10_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,		a10_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,	a10_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios,	a10_gpio_map_gpios),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_node,	a10_gpio_get_node),

        /* fdt_pinctrl interface */
	DEVMETHOD(fdt_pinctrl_configure,aw_fdt_configure_pins),

	DEVMETHOD_END
};

static devclass_t a10_gpio_devclass;

static driver_t a10_gpio_driver = {
	"gpio",
	a10_gpio_methods,
	sizeof(struct a10_gpio_softc),
};

EARLY_DRIVER_MODULE(a10_gpio, simplebus, a10_gpio_driver, a10_gpio_devclass, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);

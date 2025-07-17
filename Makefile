CC := m68k-amigaos-gcc
CXX := m68k-amigaos-g++
AS := m68k-amigaos-as

INCLUDE := -Iinclude

CFLAGS  := -m68040 -O2 -MMD -MP -Wall $(INCLUDE) -DDEBUG -Wextra # -Wno-unused-parameter
CXXFLAGS:= -m68040 -std=c++0x -O2 -MMD -MP -Wall $(INCLUDE)
ASFLAGS := -m68040
LDFLAGS := -s -nostdlib -nostartfiles

ifeq ($(MAKECMDGOALS), debug)
  CFLAGS += -g
  CXXFLAGS += -g
  CFLAGS += -DDEBUG
  CXXFLAGS += -DDEBUG
  LDFLAGS += -ldebug
endif

OBJS := device.o device_beginio.o device_abortio.o devtree.o unit.o unit_task.o unit_commands.o unit_commands_mcast.o unit_io.o genet/bcmgenet.o genet/bcmgenet-tx.o genet/bcm_gpio.o genet/phy.o genet/phy_interface.o device_end.o
OBJDIR := Build
OBJNAME := genet.device

.PHONY: all clean

all: $(OBJDIR) $(OBJDIR)/genet $(OBJDIR)/$(OBJNAME)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/genet:
	@mkdir -p $(OBJDIR)/genet

$(OBJDIR)/$(OBJNAME): $(addprefix $(OBJDIR)/, $(OBJS))
	$(CC) $(foreach f,$(OBJS),$(OBJDIR)/$(f)) $(LDFLAGS) -o $@


$(OBJDIR)/%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(OBJDIR)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(OBJDIR)/%.o: %.s
	$(AS) $(ASFLAGS) -c $< -o $@

-include $(addprefix $(OBJDIR)/, $(OBJS:.o=.d))

clean:
	@rm -rf $(OBJDIR)

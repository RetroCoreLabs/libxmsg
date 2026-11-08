/* Generated from DOC/protocols/xrout-services.json by generate_c.py. DO NOT EDIT. */
#ifndef XMSG_XROUT_SERVICES_H
#define XMSG_XROUT_SERVICES_H

#include <stdint.h>

/*
 * the phone book: it turns a registered name into an address, forwards the first letter, and
 * counts the seats
 */

/*
 * Service codes are DECIMAL here and octal in the ND manuals - the manual's XSRME=106 is 70.
 * Mixing the two bases is an easy and silent error.
 */
typedef uint16_t xrout_service_t;

/* do nothing - the no-op -- INFERRED: ND-60.134.2 Communication Guide service table */
#define XROUT_XSNUL                        64

/*
 * deliver this letter to a service I only know by NAME. XROUT looks the name up, spends one of
 * that service's free slots, and passes the letter on - after which the two sides talk
 * directly and XROUT is out of the way -- MEASURED: every chat join and FA connect; the
 * forward that spends a seat
 */
#define XROUT_XSLET                        65

/*
 * claim a name for my port, so others can find me by asking for it instead of needing my
 * address -- MEASURED: our servers claim their names this way
 */
#define XROUT_XSNAM                        66

/*
 * give my name back, so nobody is sent to a port that has closed -- MEASURED: ChatServer.Close
 * releases its name
 */
#define XROUT_XSCNM                        67

/* tell me the name registered for this address -- INFERRED: service table */
#define XROUT_XSGNM                        68

/*
 * tell me more about a registered name - who holds it and how much room it has -- INFERRED:
 * service table
 */
#define XROUT_XSGNI                        69

/*
 * find me the address of a named port on ANOTHER machine. Asked of our own XROUT, which does
 * the asking onwards -- INFERRED: Communication Guide section 4.4.2.9; manual gives it as
 * octal 106
 */
#define XROUT_XSRME                        70

/* give me the address behind this name -- INFERRED: service table */
#define XROUT_XSGMG                        71

/*
 * forget that address - one XROUT telling another that a port is gone. Privileged, and not
 * something a normal program sends -- INFERRED: Communication Guide section 4.4.2.11; octal
 * 110
 */
#define XROUT_XSCMG                        72

/*
 * record that a remote system exists and what number it has. This is what the DEF-REMOTE
 * operator command does, and it is how D100 was taught that D19999 is us -- MEASURED: used to
 * define D19999 on D100
 */
#define XROUT_XSDRN                        73

/*
 * record how to reach another machine - which link, and how far away it is -- INFERRED:
 * service table Same value as XSDSY.
 */
#define XROUT_XSDMC                        74

/*
 * record how to reach another machine - the same service as XSDMC, under a second name --
 * INFERRED: service table Same value as XSDMC.
 */
#define XROUT_XSDSY                        74

/*
 * tell me what you know about that machine - can you reach it, and how -- INFERRED: service
 * table Same value as XSGSY.
 */
#define XROUT_XSGMC                        75

/*
 * tell me what you know about that machine - the same service as XSGMC. D100 answers this one
 * live, and its reply carries a counter that changes each time, which is how we know we are
 * talking to something real rather than a cache -- MEASURED: answered live by D100; see the
 * XSGSY echo-counter note Same value as XSGMC.
 */
#define XROUT_XSGSY                        75

/*
 * tell me about the links this machine has - what is connected and in what state -- INFERRED:
 * service table
 */
#define XROUT_XSLKI                        76

/* tell me the time, as this machine has it -- INFERRED: service table */
#define XROUT_XSTIN                        77

/*
 * Close tracing (privileged) -- INFERRED: the ND service table, via the summary on the enum
 * member. Not driven against a machine here
 */
#define XROUT_XSTCL                        78

/*
 * Define tracing conditions (privileged) -- INFERRED: the ND service table, via the summary on
 * the enum member. Not driven against a machine here
 */
#define XROUT_XSTDC                        79

/*
 * Create service (name, init no of SP's) -- INFERRED: the ND service table, via the summary on
 * the enum member. Not driven against a machine here
 */
#define XROUT_XSCRS                        80

/*
 * New service point(s) (increment/decrement in SP's) -- INFERRED: the ND service table, via
 * the summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSNSP                        81

/*
 * Get information about name -- INFERRED: the ND service table, via the summary on the enum
 * member. Not driven against a machine here
 */
#define XROUT_XSGIN                        82

/*
 * Define local system (privileged) -- INFERRED: the ND service table, via the summary on the
 * enum member. Not driven against a machine here
 */
#define XROUT_XSDLO                        83

/*
 * Send letter and kick if unavailable (privileged) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSLEK                        84

/*
 * Start/stop gateway (network server) (privileged) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSNET                        85

/*
 * Set crash information (privileged) -- INFERRED: the ND service table, via the summary on the
 * enum member. Not driven against a machine here
 */
#define XROUT_XSSCI                        86

/*
 * Get/check attribute -- INFERRED: the ND service table, via the summary on the enum member.
 * Not driven against a machine here
 */
#define XROUT_XSGAT                        87

/*
 * Define/remove attribute (privileged) -- INFERRED: the ND service table, via the summary on
 * the enum member. Not driven against a machine here
 */
#define XROUT_XSDAT                        88

/*
 * Get network server information (privileged) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSNSI                        89

/*
 * Get information about a link (privileged) -- INFERRED: the ND service table, via the summary
 * on the enum member. Not driven against a machine here
 */
#define XROUT_XSLIN                        90

/*
 * Get information about named ports -- INFERRED: the ND service table, via the summary on the
 * enum member. Not driven against a machine here
 */
#define XROUT_XSPIN                        91

/*
 * Get information about a system (privileged) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSLSY                        92

/*
 * Get info about system utilization (privileged) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSGSU                        93

/*
 * Start/stop Cosmos routing manager (priv - COSROUT only!) -- INFERRED: the ND service table,
 * via the summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSCRM                        94

/*
 * Get information from link tables (priv - COSROUT only!) -- INFERRED: the ND service table,
 * via the summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSGLI                        95

/*
 * Get info about system generation variables (privileged) -- INFERRED: the ND service table,
 * via the summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSGSG                        96

/*
 * Maximum legal service value (alias of XSGSG) -- INFERRED: the ND service table, via the
 * summary on the enum member. Not driven against a machine here
 */
#define XROUT_XSMAX                        96

/*
 * There are far more of these than we produce. They are PEER-produced by nature: we need to
 * NAME one when it arrives, not to invent it. Only the handful we act on are listed.
 */
typedef int16_t xrout_error_t;

/* it worked -- MEASURED: every successful exchange */
#define XROUT_ERR_XRSOK                    0

/*
 * nobody here has registered that name - either the service is not running, or the name is
 * spelled differently -- MEASURED: returned for a name never registered
 */
#define XROUT_ERR_XRUNN                    2

/*
 * that service is full. Its free-slot count is zero, so XROUT will not forward another letter
 * to it until somebody leaves -- MEASURED: the room-full path; see the_seat_law
 */
#define XROUT_ERR_XRNSP                    4

/*
 * that address means nothing here - the port it named has closed -- MEASURED: delivery to a
 * closed port
 */
#define XROUT_ERR_XRUNM                    7

/*
 * there is no connection to work with -- MEASURED: D100 answered APPEND-REMOTE-BATCH with it,
 * task 30 - a NAMED refusal, which proves the path
 */
#define XROUT_ERR_XRNCO                    46

/*
 * Illegal service number -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRISN                    1

/*
 * Another port already has this name -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRDDF                    3

/*
 * Illegal parameter type -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRIPT                    5

/*
 * Missing mandatory parameter -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRMMP                    6

/*
 * Too short message or resulting message too long -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRMTL                    8

/*
 * Standard message format not handled -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRSMF                    9

/*
 * Caller was not privileged -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRPRV                    10

/*
 * Illegal system number parameter -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRISY                    11

/*
 * No access to remote system -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNRO                    12

/*
 * Illegal integer value -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRIIV                    13

/*
 * Cannot define route to a neighbour -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNEI                    14

/*
 * Invalid service request - not available to current caller -- INFERRED: the ND error table,
 * via the summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNXM                    15

/*
 * Illegal/Reserved Logical Unit Number (LUN) for link -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRILN                    16

/*
 * No more Link Descriptors (XL-blocks) for start-link/netserver -- INFERRED: the ND error
 * table, via the summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNXL                    17

/*
 * Not enough resources (XD/XF/XM-Blocks) for start-link/netserver -- INFERRED: the ND error
 * table, via the summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNXD                    18

/*
 * No trace generated (no trace buffer available)' -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNTR                    19

/*
 * Trace already active -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTRA                    20

/*
 * Trace passive -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTRP                    21

/*
 * Trace/dump file open error (see parameter 1) -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTFE                    22

/*
 * Trace RT-prog (XTRACE) not found -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTRT                    23

/*
 * Illegal trace system number -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTIS                    24

/*
 * Bad link - open unsuccessful -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRBLK                    25

/*
 * Attempt to redefine local system no -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRSYD                    26

/*
 * No local system number defined -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNLS                    27

/*
 * Too many remote names to this system -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRTRE                    28

/*
 * Old service calls (below 64) cannot go remote -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRNA                    29

/*
 * Service points busy -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRBUS                    30

/*
 * This is not a service port -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNSE                    31

/*
 * Remote port statically declared -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRPN                    32

/*
 * Unknown remote system name or number -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRUKS                    33

/*
 * Remote system message table space full -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRMFL                    34

/*
 * Remote task message space used up -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRROV                    35

/*
 * Routing table full (too many systems) -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRFU                    36

/*
 * No remote batch service available -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNRB                    37

/*
 * Unknown RT name -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRURT                    38

/*
 * This server is not running -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRSNR                    39

/*
 * Netserver: remote system is not defined -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRND                    40

/*
 * Netserver: network not available -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNNA                    41

/*
 * Netserver: internal server error -- INFERRED: the ND error table, via the summary on the
 * enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRISE                    42

/*
 * Netserver: invalid request -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRIRQ                    43

/*
 * XMSG not congfigurated with gateway code -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNGA                    44

/*
 * Remote system not on same LAN -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRNL                    45

/*
 * Ambiguous name -- INFERRED: the ND error table, via the summary on the enum member.
 * PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRAMB                    47

/*
 * Friend system table full (too many friends) -- INFERRED: the ND error table, via the summary
 * on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRFFU                    48

/*
 * Netserver: network temporarily not available -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRNTA                    49

/*
 * Netserver: message too old -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRMTO                    50

/*
 * COSMOS routing manager is not running -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRCNR                    51

/*
 * COSMOS routing manager is already running -- INFERRED: the ND error table, via the summary
 * on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRICR                    52

/*
 * Routing information defined (start/stop inhibited) -- INFERRED: the ND error table, via the
 * summary on the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRRID                    53

/*
 * COSMOS routing manager: invalid request -- INFERRED: the ND error table, via the summary on
 * the enum member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRIRR                    54

/*
 * XRILX - Illegal link number parameter. SOURCE DISCREPANCY: present in XMSG-PL-VALUES-L.INCL
 * ("CONSTANT XRILX=55") but ABSENT from XMSG-VALUES-L.SYMB, whose XROUT error range stops at
 * XRIRR=54. The M-version include file that generated this enum has it, so the value 55 is
 * kept -- INFERRED: the ND error table, via the summary on the enum member. PEER-produced: we
 * name these, we do not send them
 */
#define XROUT_ERR_XRILX                    55

/*
 * Base for XROUT errors: 41100B -- INFERRED: the ND error table, via the summary on the enum
 * member. PEER-produced: we name these, we do not send them
 */
#define XROUT_ERR_XRXXX                    16960

/* xrout_connection_type. */
typedef uint8_t xrout_connection_type_t;

/* Unavailable -- MEASURED: returned for a system with no route */
#define XROUT_CONN_UNAVAILABLE             0

/*
 * the machine is on the same wire - we can talk to it without being told a path -- MEASURED:
 * an adjacent system answers without one
 */
#define XROUT_CONN_NEIGHBOUR               1

/*
 * the machine is further away, and another machine passes traffic along for us -- MEASURED:
 * the 102-via-100-to-103 route-through captures
 */
#define XROUT_CONN_VIA                     2

/* ViaNetworkServer -- UNKNOWN: named in the sources; never observed on this lab */
#define XROUT_CONN_VIA_NETWORK_SERVER      3

/* Local -- MEASURED: our own system */
#define XROUT_CONN_LOCAL                   4

#endif /* XMSG_XROUT_SERVICES_H */

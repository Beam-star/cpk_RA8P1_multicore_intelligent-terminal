/*
 * usb_pcdc_descriptor.c
 *
 * USB PCDC (CDC ACM) device descriptor for Titan-mini RA8P1.
 * Virtual COM port — appears as a serial port on PC.
 *
 * Interface layout:
 *   Interface 0: CDC Control (Communication Class, 1 interrupt IN EP)
 *   Interface 1: CDC Data     (CDC Data Class,     1 bulk IN + 1 bulk OUT EP)
 */

#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
#include "r_usb_basic_cfg.h"

/* -------------------------------------------------------------------------- */
/* Sizes                                                                      */
/* -------------------------------------------------------------------------- */

#define USB_PCDC_CD1_LEN              (67U)
#define USB_PCDC_QD_LEN               (10U)
#define STRING_DESCRIPTOR0_LEN         (4U)
#define STRING_DESCRIPTOR1_LEN        (28U)  /* iManufacturer: "Titan-mini" */
#define STRING_DESCRIPTOR2_LEN        (34U)  /* iProduct: "PCDC Virtual COM" */
#define STRING_DESCRIPTOR3_LEN        (46U)  /* iInterface: "Communications Device" */
#define STRING_DESCRIPTOR4_LEN        (22U)  /* FS Config name */
#define STRING_DESCRIPTOR5_LEN        (18U)  /* HS Config name */
#define STRING_DESCRIPTOR6_LEN        (28U)  /* iSerialNumber */
#define NUM_STRING_DESCRIPTOR          (7U)

/* -------------------------------------------------------------------------- */
/* Class-Specific Constants                                                    */
/* -------------------------------------------------------------------------- */

#define USB_PCDC_CS_INTERFACE                          (0x24U)
#define USB_PCDC_DT_SUBTYPE_HEADER_FUNC                (0x00U)
#define USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC           (0x01U)
#define USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC   (0x02U)
#define USB_PCDC_DT_SUBTYPE_UNION_FUNC                 (0x06U)
#define USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL       (0x02U)
#define USB_PCDC_BCD_CDC                               (0x0110U)

#define USB_BCDNUM        (0x0200U)
#define USB_RELEASE       (0x0100U)
#define USB_DCPMAXP       (64U)
#define USB_CONFIGNUM      (1U)
#define USB_VENDORID       (0x045BU)  /* Renesas */
#define USB_PRODUCTID      (0x5001U)

#define USB_UCHAR_MAX      (0xFFU)
#define USB_W_TOTAL_LENGTH_MASK  (256U)
#define USB_W_MAX_PACKET_SIZE_MASK (64U)
#define USB_PCDC_BCD_CDC_MASK (256U)

/* -------------------------------------------------------------------------- */
/* Device Descriptor                                                          */
/* -------------------------------------------------------------------------- */

uint8_t g_apl_device[USB_DD_BLENGTH + (USB_DD_BLENGTH % 2)] =
{
    USB_DD_BLENGTH,                                                 /*  0: bLength */
    USB_DT_DEVICE,                                                  /*  1: bDescriptorType */
    (uint8_t)(USB_BCDNUM & USB_UCHAR_MAX),                          /*  2: bcdUSB_lo */
    (uint8_t)((USB_BCDNUM >> 8) & USB_UCHAR_MAX),                   /*  3: bcdUSB_hi */
    USB_IFCLS_CDCC,                                                 /*  4: bDeviceClass (CDC) */
    0x00,                                                           /*  5: bDeviceSubClass */
    0x00,                                                           /*  6: bDeviceProtocol */
    (uint8_t)USB_DCPMAXP,                                           /*  7: bMaxPacketSize0 */
    (uint8_t)(USB_VENDORID & USB_UCHAR_MAX),                        /*  8: idVendor_lo */
    (uint8_t)((USB_VENDORID >> 8) & USB_UCHAR_MAX),                 /*  9: idVendor_hi */
    (uint8_t)(USB_PRODUCTID & USB_UCHAR_MAX),                       /* 10: idProduct_lo */
    (uint8_t)((USB_PRODUCTID >> 8) & USB_UCHAR_MAX),                /* 11: idProduct_hi */
    (uint8_t)(USB_RELEASE & USB_UCHAR_MAX),                         /* 12: bcdDevice_lo */
    (uint8_t)((USB_RELEASE >> 8) & USB_UCHAR_MAX),                  /* 13: bcdDevice_hi */
    0x01,                                                           /* 14: iManufacturer */
    0x02,                                                           /* 15: iProduct */
    0x06,                                                           /* 16: iSerialNumber */
    USB_CONFIGNUM                                                   /* 17: bNumConfigurations */
};

/* -------------------------------------------------------------------------- */
/* Device Qualifier Descriptor                                                */
/* -------------------------------------------------------------------------- */

uint8_t g_apl_qualifier_descriptor[USB_PCDC_QD_LEN + (USB_PCDC_QD_LEN % 2)] =
{
    USB_PCDC_QD_LEN,                                                /*  0: bLength */
    USB_DT_DEVICE_QUALIFIER,                                        /*  1: bDescriptorType */
    (uint8_t)(USB_BCDNUM & USB_UCHAR_MAX),                          /*  2: bcdUSB_lo */
    (uint8_t)((USB_BCDNUM >> 8) & USB_UCHAR_MAX),                   /*  3: bcdUSB_hi */
    0x00,                                                           /*  4: bDeviceClass */
    0x00,                                                           /*  5: bDeviceSubClass */
    0x00,                                                           /*  6: bDeviceProtocol */
    (uint8_t)USB_DCPMAXP,                                           /*  7: bMaxPacketSize0 */
    USB_CONFIGNUM,                                                  /*  8: bNumConfigurations */
    0x00                                                            /*  9: bReserved */
};

/* -------------------------------------------------------------------------- */
/* Full-Speed Configuration Descriptor                                        */
/* -------------------------------------------------------------------------- */

uint8_t g_apl_configuration[USB_PCDC_CD1_LEN + (USB_PCDC_CD1_LEN % 2)] =
{
    /* ---- Configuration Descriptor ---- */
    USB_CD_BLENGTH,                                                 /*  0: bLength */
    USB_DT_CONFIGURATION,                                           /*  1: bDescriptorType */
    (uint8_t)(USB_PCDC_CD1_LEN % USB_W_TOTAL_LENGTH_MASK),          /*  2: wTotalLength(L) */
    (uint8_t)(USB_PCDC_CD1_LEN / USB_W_TOTAL_LENGTH_MASK),          /*  3: wTotalLength(H) */
    0x02,                                                           /*  4: bNumInterfaces */
    0x01,                                                           /*  5: bConfigurationValue */
    0x04,                                                           /*  6: iConfiguration */
    USB_CF_RESERVED | USB_CF_SELFP,                                  /*  7: bmAttributes */
    (100 / 2),                                                      /*  8: bMaxPower (100mA) */

    /* ---- Interface 0: CDC Control ---- */
    USB_ID_BLENGTH,                                                 /*  0: bLength */
    USB_DT_INTERFACE,                                               /*  1: bDescriptorType */
    0x00,                                                           /*  2: bInterfaceNumber */
    0x00,                                                           /*  3: bAlternateSetting */
    0x01,                                                           /*  4: bNumEndpoints (1 interrupt IN) */
    USB_IFCLS_CDCC,                                                 /*  5: bInterfaceClass (CDC Control) */
    USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL,                       /*  6: bInterfaceSubClass (ACM) */
    0x01,                                                           /*  7: bInterfaceProtocol (AT commands) */
    0x03,                                                           /*  8: iInterface */

        /* ---- CDC Header Functional Descriptor ---- */
        0x05,                                                       /*  0: bLength */
        USB_PCDC_CS_INTERFACE,                                      /*  1: bDescriptorType */
        USB_PCDC_DT_SUBTYPE_HEADER_FUNC,                            /*  2: bDescriptorSubtype */
        (uint8_t)(USB_PCDC_BCD_CDC % USB_W_TOTAL_LENGTH_MASK),      /*  3: bcdCDC_lo */
        (uint8_t)(USB_PCDC_BCD_CDC / USB_W_TOTAL_LENGTH_MASK),      /*  4: bcdCDC_hi */

        /* ---- CDC ACM Functional Descriptor ---- */
        0x04,                                                       /*  0: bLength */
        USB_PCDC_CS_INTERFACE,                                      /*  1: bDescriptorType */
        USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC,               /*  2: bDescriptorSubtype */
        0x02,                                                       /*  3: bmCapabilities (D1: line coding) */

        /* ---- CDC Union Functional Descriptor ---- */
        0x05,                                                       /*  0: bLength */
        USB_PCDC_CS_INTERFACE,                                      /*  1: bDescriptorType */
        USB_PCDC_DT_SUBTYPE_UNION_FUNC,                             /*  2: bDescriptorSubtype */
        0x00,                                                       /*  3: bMasterInterface (CDC Control) */
        0x01,                                                       /*  4: bSlaveInterface0 (CDC Data) */

        /* ---- CDC Call Management Functional Descriptor ---- */
        0x05,                                                       /*  0: bLength */
        USB_PCDC_CS_INTERFACE,                                      /*  1: bDescriptorType */
        USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC,                       /*  2: bDescriptorSubtype */
        0x03,                                                       /*  3: bmCapabilities (D1|D0: self-managed) */
        0x01,                                                       /*  4: bDataInterface */

        /* ---- Endpoint: Interrupt IN (CDC notifications) ---- */
        USB_ED_BLENGTH,                                             /*  0: bLength */
        USB_DT_ENDPOINT,                                            /*  1: bDescriptorType */
        USB_EP_IN | USB_EP3,                                        /*  2: bEndpointAddress */
        USB_EP_INT,                                                 /*  3: bmAttributes */
        0x10,                                                       /*  4: wMaxPacketSize (16 bytes) */
        0x00,
        0x10,                                                       /*  6: bInterval (16ms) */

    /* ---- Interface 1: CDC Data ---- */
    USB_ID_BLENGTH,                                                 /*  0: bLength */
    USB_DT_INTERFACE,                                               /*  1: bDescriptorType */
    0x01,                                                           /*  2: bInterfaceNumber */
    0x00,                                                           /*  3: bAlternateSetting */
    0x02,                                                           /*  4: bNumEndpoints (BULK IN + OUT) */
    USB_IFCLS_CDCD,                                                 /*  5: bInterfaceClass (CDC Data) */
    0x00,                                                           /*  6: bInterfaceSubClass */
    0x00,                                                           /*  7: bInterfaceProtocol */
    0x00,                                                           /*  8: iInterface */

        /* ---- Endpoint: BULK IN (device → host) ---- */
        USB_ED_BLENGTH,                                             /*  0: bLength */
        USB_DT_ENDPOINT,                                            /*  1: bDescriptorType */
        USB_EP_IN | USB_EP1,                                        /*  2: bEndpointAddress */
        USB_EP_BULK,                                                /*  3: bmAttributes */
        (uint8_t)USB_W_MAX_PACKET_SIZE_MASK,                        /*  4: wMaxPacketSize (64 bytes) */
        0x00,
        0x00,                                                       /*  6: bInterval */

        /* ---- Endpoint: BULK OUT (host → device) ---- */
        USB_ED_BLENGTH,                                             /*  0: bLength */
        USB_DT_ENDPOINT,                                            /*  1: bDescriptorType */
        USB_EP_OUT | USB_EP2,                                       /*  2: bEndpointAddress */
        USB_EP_BULK,                                                /*  3: bmAttributes */
        (uint8_t)USB_W_MAX_PACKET_SIZE_MASK,                        /*  4: wMaxPacketSize (64 bytes) */
        0x00,
        0x00,                                                       /*  6: bInterval */
};

/* -------------------------------------------------------------------------- */
/* High-Speed Configuration Descriptor (same layout, 512-byte bulk)           */
/* -------------------------------------------------------------------------- */

uint8_t g_apl_hs_configuration[USB_PCDC_CD1_LEN + (USB_PCDC_CD1_LEN % 2)] =
{
    USB_CD_BLENGTH,
    USB_DT_CONFIGURATION,
    (uint8_t)(USB_PCDC_CD1_LEN % USB_W_TOTAL_LENGTH_MASK),
    (uint8_t)(USB_PCDC_CD1_LEN / USB_W_TOTAL_LENGTH_MASK),
    0x02, 0x01, 0x04,
    USB_CF_RESERVED | USB_CF_SELFP, (100 / 2),

    /* Interface 0: CDC Control */
    USB_ID_BLENGTH, USB_DT_INTERFACE,
    0x00, 0x00, 0x01,
    USB_IFCLS_CDCC, USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL, 0x01, 0x03,

        /* CDC Functional Descriptors */
        0x05, USB_PCDC_CS_INTERFACE, USB_PCDC_DT_SUBTYPE_HEADER_FUNC,
        (uint8_t)(USB_PCDC_BCD_CDC % USB_PCDC_BCD_CDC_MASK),
        (uint8_t)(USB_PCDC_BCD_CDC / USB_PCDC_BCD_CDC_MASK),

        0x04, USB_PCDC_CS_INTERFACE, USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC, 0x02,

        0x05, USB_PCDC_CS_INTERFACE, USB_PCDC_DT_SUBTYPE_UNION_FUNC, 0x00, 0x01,

        0x05, USB_PCDC_CS_INTERFACE, USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC, 0x03, 0x01,

        /* Interrupt IN */
        USB_ED_BLENGTH, USB_DT_ENDPOINT,
        USB_EP_IN | USB_EP3, USB_EP_INT,
        0x10, 0x00, 0x10,

    /* Interface 1: CDC Data */
    USB_ID_BLENGTH, USB_DT_INTERFACE,
    0x01, 0x00, 0x02,
    USB_IFCLS_CDCD, 0x00, 0x00, 0x00,

        /* BULK IN (HS: 512 bytes) */
        USB_ED_BLENGTH, USB_DT_ENDPOINT,
        USB_EP_IN | USB_EP1, USB_EP_BULK,
        0x00, 0x02, 0x00,

        /* BULK OUT (HS: 512 bytes) */
        USB_ED_BLENGTH, USB_DT_ENDPOINT,
        USB_EP_OUT | USB_EP2, USB_EP_BULK,
        0x00, 0x02, 0x00,
};

/* -------------------------------------------------------------------------- */
/* String Descriptors                                                         */
/* -------------------------------------------------------------------------- */

uint8_t g_cdc_string0[STRING_DESCRIPTOR0_LEN + (STRING_DESCRIPTOR0_LEN % 2)] =
{
    STRING_DESCRIPTOR0_LEN, USB_DT_STRING,
    0x09, 0x04              /* English (United States) */
};

/* iManufacturer: "Titan-mini" */
uint8_t g_cdc_string1[STRING_DESCRIPTOR1_LEN + (STRING_DESCRIPTOR1_LEN % 2)] =
{
    STRING_DESCRIPTOR1_LEN, USB_DT_STRING,
    'T', 0x00, 'i', 0x00, 't', 0x00, 'a', 0x00, 'n', 0x00,
    '-', 0x00, 'm', 0x00, 'i', 0x00, 'n', 0x00, 'i', 0x00
};

/* iProduct: "PCDC Virtual COM" */
uint8_t g_cdc_string2[STRING_DESCRIPTOR2_LEN + (STRING_DESCRIPTOR2_LEN % 2)] =
{
    STRING_DESCRIPTOR2_LEN, USB_DT_STRING,
    'P', 0x00, 'C', 0x00, 'D', 0x00, 'C', 0x00, ' ', 0x00,
    'V', 0x00, 'i', 0x00, 'r', 0x00, 't', 0x00, 'u', 0x00,
    'a', 0x00, 'l', 0x00, ' ', 0x00, 'C', 0x00, 'O', 0x00,
    'M', 0x00
};

/* iInterface: "Communications Device" */
uint8_t g_cdc_string3[STRING_DESCRIPTOR3_LEN + (STRING_DESCRIPTOR3_LEN % 2)] =
{
    STRING_DESCRIPTOR3_LEN, USB_DT_STRING,
    'C', 0x00, 'o', 0x00, 'm', 0x00, 'm', 0x00, 'u', 0x00,
    'n', 0x00, 'i', 0x00, 'c', 0x00, 'a', 0x00, 't', 0x00,
    'i', 0x00, 'o', 0x00, 'n', 0x00, 's', 0x00, ' ', 0x00,
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00,
    'e', 0x00
};

/* iConfiguration (FS) */
uint8_t g_cdc_string4[STRING_DESCRIPTOR4_LEN + (STRING_DESCRIPTOR4_LEN % 2)] =
{
    STRING_DESCRIPTOR4_LEN, USB_DT_STRING,
    'F', 0x00, 'u', 0x00, 'l', 0x00, 'l', 0x00, '-', 0x00,
    'S', 0x00, 'p', 0x00, 'e', 0x00, 'e', 0x00, 'd', 0x00
};

/* iConfiguration (HS) */
uint8_t g_cdc_string5[STRING_DESCRIPTOR5_LEN + (STRING_DESCRIPTOR5_LEN % 2)] =
{
    STRING_DESCRIPTOR5_LEN, USB_DT_STRING,
    'H', 0x00, 'i', 0x00, '-', 0x00,
    'S', 0x00, 'p', 0x00, 'e', 0x00, 'e', 0x00, 'd', 0x00
};

/* iSerialNumber */
uint8_t g_cdc_string6[STRING_DESCRIPTOR6_LEN + (STRING_DESCRIPTOR6_LEN % 2)] =
{
    STRING_DESCRIPTOR6_LEN, USB_DT_STRING,
    '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00,
    '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00,
    '0', 0x00, '0', 0x00, '0', 0x00, '1', 0x00
};

/* -------------------------------------------------------------------------- */
/* String Table & USB Descriptor                                              */
/* -------------------------------------------------------------------------- */

uint8_t * g_apl_string_table[] =
{
    g_cdc_string0,  /* Language ID         */
    g_cdc_string1,  /* iManufacturer       */
    g_cdc_string2,  /* iProduct            */
    g_cdc_string3,  /* iInterface          */
    g_cdc_string4,  /* FS Config name      */
    g_cdc_string5,  /* HS Config name      */
    g_cdc_string6   /* iSerialNumber       */
};

usb_descriptor_t g_usb_descriptor =
{
    .p_device    = g_apl_device,
    .p_config_f  = g_apl_configuration,
    .p_config_h  = g_apl_hs_configuration,      /* Other-Speed (HS) config */
    .p_qualifier = g_apl_qualifier_descriptor,  /* Required for HS-capable device */
    .p_string    = g_apl_string_table,
    .num_string  = NUM_STRING_DESCRIPTOR
};

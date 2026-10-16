<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0">

   <xsl:output method="xml" indent="yes" indent-attributes="no"/>

   <!-- Purpose: Test XML serialization with disabled indentation of attributes -->

   <xsl:template name="xsl:initial-template">
      <a><b ccccccccccccccccc1="11111111111111111111111111111111"
            ccccccccccccccccc2="11111111111111111111111111111111"
            ccccccccccccccccc3="11111111111111111111111111111111"
            ccccccccccccccccc4="11111111111111111111111111111111"
            ccccccccccccccccc5="11111111111111111111111111111111"
            ccccccccccccccccc6="11111111111111111111111111111111"
            ccccccccccccccccc7="11111111111111111111111111111111"
            ccccccccccccccccc8="11111111111111111111111111111111"
            ccccccccccccccccc9="11111111111111111111111111111111"
            ccccccccccccccccd0="11111111111111111111111111111111"
            ccccccccccccccccd1="11111111111111111111111111111111"
            ccccccccccccccccd2="11111111111111111111111111111111"
            ccccccccccccccccd3="11111111111111111111111111111111"
            ccccccccccccccccd4="11111111111111111111111111111111"
            ccccccccccccccccd5="11111111111111111111111111111111"
            ccccccccccccccccd6="11111111111111111111111111111111"
            ccccccccccccccccd7="11111111111111111111111111111111"
            ccccccccccccccccd8="11111111111111111111111111111111"
            ccccccccccccccccd9="11111111111111111111111111111111"
            cccccccccccccccce1="11111111111111111111111111111111"
            cccccccccccccccce2="11111111111111111111111111111111"
            cccccccccccccccce3="11111111111111111111111111111111"
            cccccccccccccccce4="11111111111111111111111111111111"/></a>
   </xsl:template>
</xsl:transform>

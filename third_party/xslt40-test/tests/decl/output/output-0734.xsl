<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0">

   <xsl:output method="xml" indent="yes" indent-attributes="yes"/>

   <!-- Purpose: Test XML serialization with forced indentation of attributes -->

   <xsl:template name="xsl:initial-template">
      <a><b x="1" y="2"/></a>
   </xsl:template>
</xsl:transform>

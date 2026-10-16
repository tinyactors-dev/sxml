<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0">

   <xsl:output method="xml" line-ending="\r\n" indent-unit="\t" indent="yes" omit-xml-declaration="yes" indent-attributes="no"/>

   <!-- Purpose: Test XML serialization with line-ending=\r\n -->

   <xsl:template name="xsl:initial-template">
      <a><b/></a>
   </xsl:template>
</xsl:transform>

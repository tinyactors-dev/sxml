<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0">

   <!-- Purpose: Test XML serialization with line-ending, indent-unit, and indent-attributes -->

   <xsl:template name="xsl:initial-template">
      <xsl:result-document method="xml" line-ending="&#xA;" indent-unit="   " indent-attributes="no"
                            omit-xml-declaration="yes">
         <a z="3" x="2" y="1"/>
      </xsl:result-document>
   </xsl:template>
</xsl:transform>

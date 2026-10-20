<?xml version="1.0" encoding="UTF-8"?>
<!-- The printer in another style: four spaces, and braces around every block. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:print="https://example.org/print"
    exclude-result-prefixes="#all">

  <xsl:import href="print.xsl"/>

  <xsl:param name="indent" as="xs:string" select="'    '"/>

  <xsl:function name="print:braces" as="xs:boolean">
    <xsl:param name="block" as="element()"/>
    <xsl:sequence select="true()"/>
  </xsl:function>

</xsl:stylesheet>

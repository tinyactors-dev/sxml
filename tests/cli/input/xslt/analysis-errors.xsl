<?xml version="1.0"?>
<!-- Errors static analysis finds, in two template rules and a function:
     each is reported (D315, D320). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:f="urn:example:f">
  <xsl:template match="/">
    <xsl:value-of select="$undefined"/>
  </xsl:template>
  <xsl:template match="book">
    <xsl:value-of select="f:unknown(.)"/>
  </xsl:template>
  <xsl:function name="f:double">
    <xsl:param name="number"/>
    <xsl:sequence select="$numbr * 2"/>
  </xsl:function>
</xsl:stylesheet>

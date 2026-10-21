<?xml version="1.0"?>
<!-- Stylesheet parameters with "as" types: a value the param option
     gives is converted to the type (XSLT 4.0, 9.5), XTTE0590 when it
     cannot be. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:param name="count" as="xs:integer" select="1"/>
  <xsl:param name="due" as="xs:date" select="xs:date('2026-01-01')"/>
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:value-of select="$count instance of xs:integer, $count idiv 2,
                          $due instance of xs:date, $due + xs:dayTimeDuration('P1D')"/>
  </xsl:template>
</xsl:stylesheet>

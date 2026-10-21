<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>

  <xsl:template match="/traces">
    <xsl:value-of select="format-number(count(descendant::span), '#,##0')"/>
  </xsl:template>
</xsl:stylesheet>

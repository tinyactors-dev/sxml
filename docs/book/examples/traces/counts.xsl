<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>

  <xsl:template match="/traces">
    <xsl:value-of select="count(trace), count(trace//span)"/>
  </xsl:template>
</xsl:stylesheet>
